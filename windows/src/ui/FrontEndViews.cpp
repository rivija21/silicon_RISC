#include "Views.h"

namespace ui {

using namespace sp;

static std::pair<Color, Color> tokenColor(TokenKind k) {
    switch (k) {
    case TokenKind::Keyword: return {Theme::keyword, Color::hex(0xF3E4F5)};
    case TokenKind::Identifier: return {Theme::text, Color::hex(0xEEF0F5)};
    case TokenKind::Number: case TokenKind::Character: return {Theme::number, Color::hex(0xE1EDF8)};
    case TokenKind::String: return {Theme::string, Color::hex(0xF9E6DB)};
    case TokenKind::Punct: return {Theme::punct, Color::hex(0xF1EDE4)};
    case TokenKind::Eof: return {Theme::secondary, Color::hex(0xEEEEEE)};
    }
    return {Theme::text, Theme::panel};
}

// MARK: - Overview (station 1)

OverviewView::OverviewView() {
    addSubview(&panel);
    panel.list.onClick = [this](int, const Row& r) {
        if (r.tag >= 0 && r.tag < sp::stationCount && model) model->setStation((Station)r.tag);
    };
}

void OverviewView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    if (!(e == ModelEvent::Compiled || panel.list.rows().empty() || e == ModelEvent::Stepped)) return;
    CompilationP cp = m.compilation();
    if (!cp) return;
    const Compilation& c = *cp;
    std::vector<Row> rows;
    std::string detail = c.isAssembly ? "hand-written assembly"
                         : c.options.passes.empty() ? "compiled at -O0"
                         : c.options.passes.size() == allPasses().size() ? "compiled at -O1"
                                                                          : "custom optimisation passes";
    rows.push_back(Row::header(m.exampleName.empty() ? "Your program" : m.exampleName, detail));
    std::optional<Stage> failed = c.failedStage;
    auto stage = [&](int n, std::optional<Station> st, const std::string& name, const std::string& det, std::optional<Stage> stg, bool ok) {
        std::optional<double> t;
        if (stg)
            for (auto& [s, secs] : c.timings)
                if (s == *stg) {
                    t = secs;
                    break;
                }
        bool failedHere = stg && failed && *failed == *stg;
        std::string mark = ok ? "✓" : (failedHere ? "✗" : "·");
        Color markColor = ok ? Theme::green : failedHere ? Theme::crimson : Theme::faint;
        Row r({Cell(AttrString(mark, Fonts::ui(13, Weight::Bold), markColor), 26.0f, Align::Center),
               Cell(AttrString(cat(n, "  ", name), Fonts::ui(12.5f, Weight::Semibold), ok ? Theme::text : Theme::faint), 190.0f),
               Cell(det, Fonts::small, ok ? Theme::text : Theme::faint),
               Cell(t ? format("%.1f ms", *t * 1000) : "", Fonts::mono(10), Theme::faint, 70.0f, Align::Right)},
              Row::Kind::Normal, 30);
        r.tag = st ? (int)*st : -1;
        if (st) r.tooltip = cat("Open the ", stationTitle(*st), " view");
        rows.push_back(r);
    };
    auto plural = [](int n, const char* word) { return cat(n, " ", word, n == 1 ? "" : "s"); };
    if (!c.isAssembly) {
        bool lexOK = c.lex && !c.lex->hasErrors();
        stage(1, Station::Tokens, "Lexer",
              c.lex ? cat((int)c.lex->tokens.size() - 1, " tokens · ", (int)c.lex->macros.size(), " #define macro",
                          c.lex->macros.size() == 1 ? "" : "s")
                    : "",
              Stage::Lexer, lexOK);
        stage(2, Station::Tree, "Parser",
              c.program ? cat(c.program->nodeCount, " syntax-tree nodes · ", plural((int)c.program->functions().size(), "function"), " · ",
                              plural((int)c.program->globals().size(), "global"))
                        : "",
              Stage::Parser, c.program != nullptr);
        int warns = 0;
        for (auto& d : c.diagnostics)
            if (d.severity == Diagnostic::Severity::Warning) warns++;
        stage(3, Station::Symbols, "Semantic analysis",
              c.sema ? cat((int)c.sema->symbols.size(), " symbols · ", plural((int)c.sema->strings.size(), "string literal"),
                           warns > 0 ? cat(" · ", plural(warns, "warning")) : std::string(""))
                     : "",
              Stage::Semantic, c.sema && !c.sema->hasErrors());
        if (!c.snapshots.empty()) {
            int first = c.snapshots.front().instructions, last = c.snapshots.back().instructions;
            int pct = first > 0 ? 100 * (first - last) / first : 0;
            stage(4, Station::IR, "IR & optimiser",
                  cat(first, " → ", last, " IR instructions (", pct, "% fewer) · ", (int)c.options.passes.size(), " of ",
                      (int)allPasses().size(), " passes on"),
                  Stage::IR, true);
        } else {
            stage(4, Station::IR, "IR & optimiser", "", Stage::IR, false);
        }
        int vregs = 0, spills = 0;
        std::set<int> saved;
        for (auto& [n, a] : c.allocations) {
            vregs += (int)a.intervals.size();
            spills += a.spillSlots;
            saved.insert(a.usedCalleeSaved.begin(), a.usedCalleeSaved.end());
        }
        stage(5, Station::Asm, "Register allocation",
              c.asmLines.empty() ? ""
                                 : cat(vregs, " virtual registers → real registers · ", spills, " spilled · ",
                                       plural((int)saved.size(), "callee-saved register"), " used"),
              Stage::Codegen, !c.asmLines.empty());
        int asmCount = 0;
        for (auto& l : c.asmLines)
            if (l.kind == AsmLine::Kind::Instruction) asmCount++;
        int nf = c.ir ? (int)c.ir->functions.size() : 0;
        stage(6, Station::Asm, "Code generation", c.asmLines.empty() ? "" : cat(asmCount, " assembly instructions for ", plural(nf, "function")),
              Stage::Codegen, !c.asmLines.empty());
    }
    if (c.object) {
        int pseudo = 0;
        for (auto& l : c.object->listing)
            if (l.pseudo) pseudo++;
        stage(c.isAssembly ? 1 : 7, Station::Machine, "Assembler",
              cat((int)c.object->listing.size(), " machine instructions (", pseudo, " from pseudo-instructions) · ", (int)c.object->text.size(),
                  " B code · ", (int)c.object->data.size(), " B data · ", (int)c.object->relocations.size(), " relocations"),
              Stage::Assembler, !c.object->hasErrors());
    } else {
        stage(c.isAssembly ? 1 : 7, Station::Machine, "Assembler", "", Stage::Assembler, false);
    }
    if (c.executable) {
        std::vector<std::string> libs;
        for (auto& o : c.executable->objects)
            if (o.isLibrary) libs.push_back(o.name);
        stage(c.isAssembly ? 2 : 8, Station::Link, "Linker",
              cat((int)c.executable->objects.size(), " objects (", join(libs, ", "), ") · ", (int)c.executable->text.size(),
                  " B code at 0x00400000 · ", (int)c.executable->data.size(), " B data"),
              Stage::Linker, true);
    } else {
        stage(c.isAssembly ? 2 : 8, Station::Link, "Linker", "", Stage::Linker, false);
    }
    if (Pipeline* p = m.pipeline()) {
        const PipelineStats& s = p->stats();
        std::string state = p->halted() ? (p->output().fault ? "stopped with a fault" : "finished")
                            : (s.cycles == 0 ? "ready — press Run or Step" : "running");
        stage(c.isAssembly ? 3 : 9, Station::Pipeline, "Pipeline",
              cat(state, " · ", formatCount(s.cycles), " cycles · ", formatCount(s.retired), " instructions",
                  s.retired > 0 ? format(" · CPI %.2f", s.cpi()) : std::string("")),
              std::nullopt, true);
        const MemoryHierarchy& h = p->hierarchy;
        if (h.config.enabled) {
            stage(c.isAssembly ? 4 : 10, Station::Memory, "Memory hierarchy",
                  format("I-cache %.1f%% hits · D-cache %.1f%% hits", 100 * h.l1i.stats().hitRate(), 100 * h.l1d.stats().hitRate()) +
                      (h.l2 ? format(" · L2 %.1f%%", 100 * h.l2->stats().hitRate()) : std::string("")),
                  std::nullopt, true);
        }
    }
    std::vector<Diagnostic> problems;
    for (auto& d : c.diagnostics)
        if (d.severity != Diagnostic::Severity::Note) problems.push_back(d);
    if (!problems.empty()) {
        rows.push_back(Row::spacer(10));
        rows.push_back(Row::header("Problems", "click one to jump to the line"));
        for (auto& d : problems) {
            bool err = d.severity == Diagnostic::Severity::Error;
            Row r({Cell(AttrString(err ? "error" : "warning", Fonts::ui(10.5f, Weight::Bold), err ? Theme::crimson : Theme::amber), 64.0f,
                        Align::Left, err ? Theme::errorFill : Color::hex(0xFFF1C9)),
                   Cell(d.line > 0 ? cat("line ", d.line) : "", Fonts::mono(10.5f), Theme::secondary, 64.0f),
                   Cell(AttrString(cat(stageName(d.stage), ": ", d.message), Fonts::small, Theme::text))},
                  Row::Kind::Normal, 24);
            r.line = d.line;
            r.tooltip = d.message;
            rows.push_back(r);
        }
    }
    if (Pipeline* p = m.pipeline(); p && !p->output().text.empty()) {
        rows.push_back(Row::spacer(10));
        rows.push_back(Row::header("Program output so far"));
        auto lines = split(p->output().text, '\n');
        for (int k = 0; k < (int)lines.size() && k < 40; k++) rows.push_back(Row({Cell(lines[k], Fonts::mono(11.5f), Theme::text)}, Row::Kind::Normal, 17));
    }
    panel.set(rows);
}

// MARK: - Tokens

void TokenCanvas::layoutChips(float width) {
    chips.clear();
    lineLabels.clear();
    float x = 54, y = 54;
    int curLine = -1;
    Font font = Fonts::mono(11.5f);
    for (auto& t : tokens) {
        if (t.kind == TokenKind::Eof) continue;
        std::string label = t.text;
        if (t.kind == TokenKind::String) label = "\"" + CodeGen::escape(t.bytes) + "\"";
        if (charCount(label) > 40) label = prefixChars(label, 38) + "…\"";
        float w = textSize(label, font).w + 12;
        if (t.line != curLine) {
            if (curLine != -1) y += 30;
            curLine = t.line;
            x = 54;
            lineLabels.push_back({t.line, y});
        } else if (x + w > width - 12) {
            x = 54;
            y += 30;
        }
        chips.push_back({Rect(x, y, w, 22), t, label});
        x += w + 5;
    }
    contentHeight = y + 50;
}

void TokenCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(dirty, Theme::panel);
    drawText(summary, Point(14, 9), Fonts::small, Theme::secondary);
    // Legend.
    float lx = 14;
    std::pair<const char*, TokenKind> legend[] = {{"keyword", TokenKind::Keyword},
                                                  {"identifier", TokenKind::Identifier},
                                                  {"number / char", TokenKind::Number},
                                                  {"string", TokenKind::String},
                                                  {"punctuation", TokenKind::Punct}};
    for (auto& [name, k] : legend) {
        auto [fg, bg] = tokenColor(k);
        Rect r = drawPill(name, Point(lx, 28), Fonts::tiny, bg, fg);
        lx = r.maxX() + 6;
    }
    drawPill("from a #define", Point(lx, 28), Fonts::tiny, Color::hex(0xFFF1C9), Theme::directive);
    for (auto& [l, y] : lineLabels) {
        if (!(y < dirty.maxY() + 30 && y > dirty.minY() - 30)) continue;
        bool sel = selectedLine && l == *selectedLine;
        drawText(std::to_string(l), Rect(8, y, 36, 22), Fonts::mono(10.5f, sel ? Weight::Bold : Weight::Regular), sel ? Theme::heading : Theme::faint,
                 Align::Right);
    }
    Font font = Fonts::mono(11.5f);
    Rect area = dirty.insetBy(-4, -4);
    for (int k = 0; k < (int)chips.size(); k++) {
        const Chip& c = chips[k];
        if (!c.rect.intersects(area)) continue;
        auto [fg, bg] = tokenColor(c.token.kind);
        if (c.token.fromMacro) bg = Color::hex(0xFFF1C9);
        fillRounded(c.rect, 5, bg);
        bool sel = selectedLine && c.token.line == *selectedLine;
        if (sel || hovered_ == k)
            strokeRounded(c.rect, 5, sel ? Theme::selectionStrong.blended(0.3f, Theme::amber) : Theme::borderStrong, 1.5f);
        if (c.token.kind == TokenKind::Keyword) fg = Theme::keyword;
        drawText(c.label, c.rect.insetBy(6, 0), c.token.kind == TokenKind::Keyword ? Fonts::mono(11.5f, Weight::Semibold) : font, fg);
    }
    if (hovered_ && *hovered_ < (int)chips.size()) {
        const Chip& c = chips[*hovered_];
        std::string info = cat(tokenKindName(c.token.kind), "  ·  line ", c.token.line, ", column ", c.token.column);
        if (c.token.kind == TokenKind::Number || c.token.kind == TokenKind::Character) info += cat("  ·  value ", c.token.value);
        if (c.token.kind == TokenKind::String) info += cat("  ·  ", (int)c.token.bytes.size(), " bytes + terminating 0");
        if (c.token.fromMacro) info += "  ·  expanded from #define " + *c.token.fromMacro;
        Size sz = textSize(info, Fonts::small);
        Rect r{c.rect.minX(), c.rect.maxY() + 3, sz.w + 14, sz.h + 6};
        if (r.maxX() > width() - 6) r.x = width() - 6 - r.w;
        drawCard(r, 5, Color::hex(0x2B2A33));
        drawText(info, Point(r.minX() + 7, r.minY() + 3), Fonts::small, Color::white());
    }
}

void TokenCanvas::mouseMoved(const MouseEvent& e) {
    Point p = localPoint(e);
    std::optional<int> h;
    for (int k = 0; k < (int)chips.size(); k++)
        if (chips[k].rect.contains(p)) {
            h = k;
            break;
        }
    if (h != hovered_) {
        hovered_ = h;
        setNeedsDisplay();
    }
}

void TokenCanvas::mouseExited(const MouseEvent&) {
    if (hovered_) {
        hovered_.reset();
        setNeedsDisplay();
    }
}

void TokenCanvas::mouseDown(const MouseEvent& e) {
    Point p = localPoint(e);
    for (auto& c : chips)
        if (c.rect.contains(p)) {
            if (onClick) onClick(c.token);
            return;
        }
}

TokensView::TokensView() {
    addSubview(&panel);
    addSubview(&empty);
    canvas.onClick = [this](const Token& t) {
        if (!model) return;
        Selection s;
        s.line = t.line;
        s.range = t.range;
        s.origin = Station::Tokens;
        model->setSelection(s);
    };
}

void TokensView::layout() {
    panel.setFrame(bounds());
    empty.setFrame(bounds());
    if (std::abs(width() - lastWidth_) > 1) {
        lastWidth_ = width();
        canvas.layoutChips(width() - 16);
        panel.setNeedsLayout();
    }
}

void TokensView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    CompilationP c = m.compilation();
    if (!c || !c->lex || m.isAssembly()) {
        empty.setHidden(false);
        empty.title = "No tokens";
        empty.message = m.isAssembly() ? "Hand-written assembly skips the compiler front end." : "The lexer could not run.";
        empty.setNeedsDisplay();
        return;
    }
    empty.setHidden(true);
    const LexResult& lx = *c->lex;
    if (e == ModelEvent::Compiled || canvas.tokens.empty()) {
        canvas.tokens = lx.tokens;
        int comments = 0, directives = 0, macroToks = 0;
        for (auto& t : lx.trivia) (t.kind == Trivia::Kind::Comment ? comments : directives)++;
        for (auto& t : lx.tokens)
            if (t.fromMacro) macroToks++;
        canvas.summary = cat((int)lx.tokens.size() - 1, " tokens · ", comments, " comment", comments == 1 ? "" : "s", " and ", directives,
                             " preprocessor lines thrown away · ", macroToks, " tokens came from #define");
        canvas.layoutChips(width() - 16);
        panel.setNeedsLayout();
        panel.layoutSubtreeIfNeeded();
    }
    canvas.selectedLine = m.selection().line;
    canvas.setNeedsDisplay();
    if (m.selection().origin != Station::Tokens && m.selection().line) {
        for (auto& c2 : canvas.chips)
            if (c2.token.line == *m.selection().line) {
                canvas.scrollToVisible(c2.rect.insetBy(0, -40));
                break;
            }
    }
}

// MARK: - Syntax tree

static std::pair<std::string, std::vector<std::pair<std::string, ExprP>>> describeExpr(const Expr& e) {
    switch (e.kind) {
    case ExprKind::Number: return {cat("Number ", e.number), {}};
    case ExprKind::String: return {"String \"" + prefixChars(CodeGen::escape(e.bytes), 30) + "\"", {}};
    case ExprKind::Ident: return {"Name " + e.name, {}};
    case ExprKind::Binary: return {cat("Binary ", binaryOpText(e.bop)), {{"", e.a()}, {"", e.b()}}};
    case ExprKind::Unary: {
        static const std::map<UnaryOp, const char*> names = {{UnaryOp::Neg, "Negate -"},        {UnaryOp::Plus, "Plus +"},
                                                             {UnaryOp::Not, "Not !"},           {UnaryOp::BitNot, "Complement ~"},
                                                             {UnaryOp::Deref, "Dereference *"}, {UnaryOp::Addr, "Address-of &"}};
        return {names.at(e.uop), {{"", e.a()}}};
    }
    case ExprKind::Assign: return {cat("Assign ", e.hasOp ? binaryOpText(e.bop) : "", "="), {{"target", e.a()}, {"value", e.b()}}};
    case ExprKind::IncDec: return {std::string(e.isPrefix ? "Prefix " : "Postfix ") + (e.isIncrement ? "++" : "--"), {{"", e.a()}}};
    case ExprKind::Call: {
        std::vector<std::pair<std::string, ExprP>> kids;
        for (int k = 0; k < (int)e.kids.size(); k++) kids.push_back({cat("arg ", k + 1), e.kids[k]});
        return {"Call " + e.name, kids};
    }
    case ExprKind::Index: return {"Index [ ]", {{"array", e.a()}, {"index", e.b()}}};
    case ExprKind::Member: return {cat("Member ", e.arrow ? "->" : ".", e.name), {{"", e.a()}}};
    case ExprKind::Ternary: return {"Conditional ?:", {{"cond", e.a()}, {"then", e.b()}, {"else", e.c()}}};
    case ExprKind::SizeofType: return {"sizeof(" + e.ctype.description() + ")", {}};
    case ExprKind::SizeofExpr: return {"sizeof", {{"", e.a()}}};
    case ExprKind::Cast: return {"Cast (" + e.ctype.description() + ")", {{"", e.a()}}};
    case ExprKind::Comma: return {"Comma ,", {{"", e.a()}, {"", e.b()}}};
    }
    return {"?", {}};
}

std::vector<TreeNode> buildTree(const Program& p) {
    std::vector<TreeNode> out;
    auto add = [&](int depth, const std::string& role, const std::string& label, const std::string& detail, int cat_, int id, SourceRange r,
                   int line, int kids, bool last, const std::vector<bool>& anc) {
        out.push_back(TreeNode{depth, role, label, detail, cat_, id, r, line, kids > 0, last, anc});
    };
    auto plus = [](std::vector<bool> v, bool b) {
        v.push_back(b);
        return v;
    };
    std::function<void(const Expr&, int, const std::string&, bool, const std::vector<bool>&)> expr;
    std::function<void(const Stmt&, int, const std::string&, bool, const std::vector<bool>&)> stmt;
    std::function<void(const VarDecl&, int, const std::string&, bool, const std::vector<bool>&)> varDecl;
    expr = [&](const Expr& e, int d, const std::string& role, bool last, const std::vector<bool>& anc) {
        auto [label, kids] = describeExpr(e);
        add(d, role, label, ": " + e.type.description(), 2, e.id, e.range, e.line, (int)kids.size(), last, anc);
        for (int k = 0; k < (int)kids.size(); k++) expr(*kids[k].second, d + 1, kids[k].first, k == (int)kids.size() - 1, plus(anc, last));
    };
    stmt = [&](const Stmt& s, int d, const std::string& role, bool last, const std::vector<bool>& anc) {
        // Children: (role, expr | stmt | decl | case).
        struct Kid {
            std::string role;
            ExprP e;
            StmtP s;
            VarDeclP v;
            SwitchCaseP c;
        };
        std::vector<Kid> kids;
        std::string label;
        switch (s.kind) {
        case StmtKind::Block:
            label = "Block";
            for (auto& i : s.items) kids.push_back({"", nullptr, i});
            break;
        case StmtKind::Decl:
            label = "Declaration";
            for (auto& v : s.decls) kids.push_back({"", nullptr, nullptr, v});
            break;
        case StmtKind::Expr:
            label = "Expression statement";
            kids.push_back({"", s.expr});
            break;
        case StmtKind::IfElse:
            label = "If";
            kids.push_back({"cond", s.expr});
            kids.push_back({"then", nullptr, s.body});
            if (s.elseBody) kids.push_back({"else", nullptr, s.elseBody});
            break;
        case StmtKind::While:
            label = "While";
            kids.push_back({"cond", s.expr});
            kids.push_back({"body", nullptr, s.body});
            break;
        case StmtKind::DoWhile:
            label = "Do-while";
            kids.push_back({"body", nullptr, s.body});
            kids.push_back({"cond", s.expr});
            break;
        case StmtKind::For:
            label = "For";
            if (s.init) kids.push_back({"init", nullptr, s.init});
            if (s.cond) kids.push_back({"cond", s.cond});
            if (s.step) kids.push_back({"step", s.step});
            kids.push_back({"body", nullptr, s.body});
            break;
        case StmtKind::Switch:
            label = "Switch";
            kids.push_back({"value", s.expr});
            for (auto& c : s.cases) kids.push_back({"", nullptr, nullptr, nullptr, c});
            break;
        case StmtKind::Return:
            label = "Return";
            if (s.expr) kids.push_back({"", s.expr});
            break;
        case StmtKind::Break: label = "Break"; break;
        case StmtKind::Continue: label = "Continue"; break;
        case StmtKind::Empty: label = "Empty"; break;
        }
        add(d, role, label, "", 1, s.id, s.range, s.line, (int)kids.size(), last, anc);
        for (int k = 0; k < (int)kids.size(); k++) {
            bool l = k == (int)kids.size() - 1;
            const Kid& c = kids[k];
            if (c.e) expr(*c.e, d + 1, c.role, l, plus(anc, last));
            else if (c.s) stmt(*c.s, d + 1, c.role, l, plus(anc, last));
            else if (c.v) varDecl(*c.v, d + 1, c.role, l, plus(anc, last));
            else if (c.c) {
                const SwitchCase& sc = *c.c;
                std::string lab = sc.value ? cat("Case ", sc.constant) : "Default";
                add(d + 1, "", lab, "", 1, -sc.line, sc.range, sc.line, (int)sc.body.size(), l, plus(anc, last));
                for (int j = 0; j < (int)sc.body.size(); j++) stmt(*sc.body[j], d + 2, "", j == (int)sc.body.size() - 1, plus(plus(anc, last), l));
            }
        }
    };
    varDecl = [&](const VarDecl& v, int d, const std::string& role, bool last, const std::vector<bool>& anc) {
        ExprP kid;
        if (v.initializer && !v.initializer->isList) kid = v.initializer->expr;
        bool isList = v.initializer && v.initializer->isList;
        add(d, role, "Variable " + v.name, ": " + v.type.description() + (isList ? " = {…}" : ""), 0, v.id, v.range, v.line, kid ? 1 : 0, last,
            anc);
        if (kid) expr(*kid, d + 1, "init", true, plus(anc, last));
    };
    for (int k = 0; k < (int)p.items.size(); k++) {
        const TopLevel& item = p.items[k];
        bool last = k == (int)p.items.size() - 1;
        switch (item.kind) {
        case TopLevel::Kind::Function: {
            const FunctionDecl& f = *item.function;
            std::vector<std::string> ps;
            for (auto& pd : f.params) ps.push_back(pd->type.description());
            std::string sig = "(" + join(ps, ", ") + ") → " + f.returnType.description();
            add(0, "", "Function " + f.name, sig, 0, f.id, f.range, f.line, (int)f.params.size() + (f.body ? 1 : 0), last, {});
            for (int j = 0; j < (int)f.params.size(); j++) {
                const VarDecl& pd = *f.params[j];
                add(1, "param", pd.name.empty() ? "Parameter" : "Parameter " + pd.name, ": " + pd.type.description(), 0, pd.id, pd.range, pd.line, 0,
                    j == (int)f.params.size() - 1 && !f.body, {last});
            }
            if (f.body) stmt(*f.body, 1, "body", true, {last});
            break;
        }
        case TopLevel::Kind::Globals:
            for (int j = 0; j < (int)item.globals.size(); j++) varDecl(*item.globals[j], 0, "global", last && j == (int)item.globals.size() - 1, {});
            break;
        case TopLevel::Kind::StructDef: {
            const StructDef& s = *item.structDef;
            add(0, "", "Struct " + s.name, cat(s.size, " bytes"), 3, -1000 - s.id, item.range, item.line, (int)s.fields.size(), last, {});
            for (int j = 0; j < (int)s.fields.size(); j++) {
                const auto& fld = s.fields[j];
                add(1, "field", fld.name, cat(": ", fld.type.description(), "  @ offset ", fld.offset), 3, -2000 - s.id * 100 - j, item.range,
                    item.line, 0, j == (int)s.fields.size() - 1, {last});
            }
            break;
        }
        }
    }
    return out;
}

void TreeCanvas::recompute() {
    visible_.clear();
    std::optional<int> skipDepth;
    for (int k = 0; k < (int)nodes.size(); k++) {
        const TreeNode& n = nodes[k];
        if (skipDepth) {
            if (n.depth > *skipDepth) continue;
            skipDepth.reset();
        }
        visible_.push_back(k);
        if (collapsed.count(n.node) && n.hasChildren) skipDepth = n.depth;
    }
    if (hovered_ && *hovered_ >= (int)visible_.size()) hovered_.reset();
    setNeedsDisplay();
}

Size TreeCanvas::intrinsicSize() {
    int maxDepth = 0;
    for (int k : visible_) maxDepth = std::max(maxDepth, nodes[k].depth);
    return {60 + maxDepth * indentW + 520, visible_.size() * rowH + 30};
}

void TreeCanvas::reveal(std::optional<int> node, std::optional<int> line) {
    for (int v = 0; v < (int)visible_.size(); v++) {
        const TreeNode& n = nodes[visible_[v]];
        if ((node && n.node == *node) || (!node && line && n.line == *line)) {
            scrollToVisible({0, yOfVisible(v) - 60, 10, 140});
            return;
        }
    }
}

void TreeCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(dirty, Theme::panel);
    std::pair<Color, Color> cats[] = {
        {Color::hex(0xE6EEF9), Theme::ink},      // declarations
        {Color::hex(0xF5E8DE), Theme::heading},  // statements
        {Color::hex(0xE5F1EA), Theme::green},    // expressions
        {Color::hex(0xF1E6F4), Theme::purple},   // structs
    };
    for (int v = 0; v < (int)visible_.size(); v++) {
        const TreeNode& n = nodes[visible_[v]];
        float y = yOfVisible(v);
        if (y + rowH < dirty.minY() - rowH || y > dirty.maxY() + rowH) continue;
        float x = 20 + n.depth * indentW;
        // Connectors: a vertical line for every open ancestor, and an elbow into this node.
        for (int d = 0; d < (int)n.ancestorsLast.size(); d++) {
            if (d == 0 || n.ancestorsLast[d]) continue;
            float ax = 20 + (d - 1) * indentW + 8;
            line({ax, y}, {ax, y + rowH}, Theme::borderStrong);
        }
        if (n.depth > 0) {
            float px = 20 + (n.depth - 1) * indentW + 8;
            line({px, y}, {px, n.isLast ? y + rowH / 2 : y + rowH}, Theme::borderStrong);
            line({px, y + rowH / 2}, {x, y + rowH / 2}, Theme::borderStrong);
        }
        bool selected = (selectedNode && n.node == *selectedNode) || (!selectedNode && selectedLine && *selectedLine == n.line && n.category != 3);
        auto [bg, fg] = cats[n.category];
        float tx = x + 4;
        if (n.hasChildren) {
            float cx = x + 2, cy = y + rowH / 2;
            if (collapsed.count(n.node)) g.fillPolygon({Point(cx - 2, cy - 4), Point(cx + 3, cy), Point(cx - 2, cy + 4)}, Theme::secondary);
            else g.fillPolygon({Point(cx - 4, cy - 2), Point(cx + 4, cy - 2), Point(cx, cy + 3)}, Theme::secondary);
            tx += 8;
        }
        if (!n.role.empty()) {
            std::string s = uppercased(n.role);
            float w = textSize(s, Fonts::mono(8.5f, Weight::Semibold)).w;
            drawText(s, Point(tx, y + 6), Fonts::mono(8.5f, Weight::Semibold), Theme::faint);
            tx += w + 5;
        }
        float lw = textSize(n.label, Fonts::ui(12, Weight::Semibold)).w;
        Rect box{tx, y + 2.5f, lw + 14, rowH - 5};
        fillRounded(box, 5, selected ? Theme::selection : bg);
        if (selected) strokeRounded(box, 5, Theme::amber, 1.5f);
        if (hovered_ == v && !selected) strokeRounded(box, 5, Theme::borderStrong);
        drawText(n.label, Rect(box.minX() + 7, y, lw + 4, rowH), Fonts::ui(12, Weight::Semibold), fg);
        if (!n.detail.empty())
            drawText(n.detail, Rect(box.maxX() + 6, y, width() - box.maxX() - 60, rowH), Fonts::mono(11), n.category == 2 ? Theme::type : Theme::secondary);
        drawText(std::to_string(n.line), Rect(width() - 46, y, 36, rowH), Fonts::mono(10), Theme::faint, Align::Right);
    }
}

std::optional<int> TreeCanvas::visibleIndexAt(Point p) const {
    int v = (int)std::floor((p.y - 14) / rowH);
    if (v >= 0 && v < (int)visible_.size()) return v;
    return std::nullopt;
}

void TreeCanvas::mouseMoved(const MouseEvent& e) {
    auto h = visibleIndexAt(localPoint(e));
    if (h != hovered_) {
        hovered_ = h;
        setNeedsDisplay();
    }
}

void TreeCanvas::mouseExited(const MouseEvent&) {
    if (hovered_) {
        hovered_.reset();
        setNeedsDisplay();
    }
}

void TreeCanvas::mouseDown(const MouseEvent& e) {
    Point p = localPoint(e);
    auto v = visibleIndexAt(p);
    if (!v) return;
    const TreeNode n = nodes[visible_[*v]];
    float x = 20 + n.depth * indentW;
    if (n.hasChildren && (p.x < x + 12 || e.clickCount == 2)) {
        if (collapsed.count(n.node)) collapsed.erase(n.node);
        else collapsed.insert(n.node);
        recompute();
        if (superview() && superview()->superview()) superview()->superview()->setNeedsLayout();
        if (auto* s = enclosingScrollView())
            if (s->superview()) s->superview()->setNeedsLayout();
        return;
    }
    if (onClick) onClick(n);
}

TreeView::TreeView() {
    for (View* v : std::initializer_list<View*>{&panel, &expand, &collapseB, &legend, &empty}) addSubview(v);
    canvas.onClick = [this](const TreeNode& n) {
        if (!model) return;
        Selection s;
        s.line = n.line;
        s.node = n.node;
        s.range = n.range;
        s.origin = Station::Tree;
        model->setSelection(s);
    };
    expand.action = [this] {
        canvas.collapsed.clear();
        canvas.recompute();
        panel.setNeedsLayout();
    };
    collapseB.action = [this] {
        // Collapse every statement's expression subtree.
        canvas.collapsed.clear();
        for (auto& n : canvas.nodes)
            if (hasPrefix(n.label, "Expression") || n.label == "Return" || n.label == "Declaration") canvas.collapsed.insert(n.node);
        canvas.recompute();
        panel.setNeedsLayout();
    };
}

void TreeView::layout() {
    Size e = expand.intrinsicSize(), c = collapseB.intrinsicSize();
    expand.setFrame({10, 8, e.w, 24});
    collapseB.setFrame({expand.frame().maxX() + 6, 8, c.w, 24});
    legend.setFrame({collapseB.frame().maxX() + 12, 12, width() - collapseB.frame().maxX() - 20, 16});
    panel.setFrame({0, 40, width(), height() - 40});
    empty.setFrame(bounds());
}

void TreeView::draw(Gfx& g, const Rect& dirty) { fillRect(bounds(), Theme::panel); }

void TreeView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    ProgramP prog;
    if (m.compilation() && m.compilation()->program) prog = m.compilation()->program;
    else if (!m.isAssembly() && m.good()) prog = m.good()->program;
    if (!prog || m.isAssembly()) {
        empty.setHidden(false);
        empty.title = "No syntax tree";
        empty.message = m.isAssembly() ? "Hand-written assembly skips the parser." : "The parser stopped at a syntax error — see Problems under the editor.";
        empty.setNeedsDisplay();
        return;
    }
    empty.setHidden(true);
    if (e == ModelEvent::Compiled || canvas.nodes.empty()) {
        canvas.nodes = buildTree(*prog);
        canvas.recompute();
        panel.setNeedsLayout();
        panel.layoutSubtreeIfNeeded();
    }
    canvas.selectedLine = m.selection().line;
    canvas.selectedNode = m.selection().origin == Station::Tree ? m.selection().node : std::nullopt;
    canvas.setNeedsDisplay();
    if (m.selection().origin != Station::Tree && m.selection().line) canvas.reveal(std::nullopt, m.selection().line);
}

// MARK: - Symbols

SymbolsView::SymbolsView() {
    addSubview(&panel);
    addSubview(&empty);
    panel.list.onClick = [this](int, const Row& r) {
        if (model) model->selectLine(r.line, Station::Symbols);
    };
    panel.list.highlight = [this](const Row& r) -> std::optional<Color> {
        if (model && model->selection().line && r.line == model->selection().line) return Theme::selection;
        return std::nullopt;
    };
}

void SymbolsView::layout() {
    panel.setFrame(bounds());
    empty.setFrame(bounds());
}

void SymbolsView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    CompilationP c = m.shown();
    if (m.isAssembly() || !c || !c->sema || !c->program) {
        empty.setHidden(false);
        empty.title = "No symbol table";
        empty.message = m.isAssembly() ? "Hand-written assembly has labels instead — see the Linking view." : "Semantic analysis did not run.";
        empty.setNeedsDisplay();
        return;
    }
    empty.setHidden(true);
    if (!(e == ModelEvent::Compiled || panel.list.rows().empty() || e == ModelEvent::Selection)) return;
    if (e == ModelEvent::Selection) {
        panel.list.setNeedsDisplay();
        return;
    }
    const SemaResult& sema = *c->sema;
    const Program& prog = *c->program;
    std::vector<Row> rows;
    const float cols[] = {130, 80, 170, 60};
    auto headerRow = [&] {
        Row r({Cell("NAME", Fonts::mono(9.5f, Weight::Semibold), Theme::faint, cols[0]), Cell("KIND", Fonts::mono(9.5f, Weight::Semibold), Theme::faint, cols[1]),
               Cell("TYPE", Fonts::mono(9.5f, Weight::Semibold), Theme::faint, cols[2]), Cell("SIZE", Fonts::mono(9.5f, Weight::Semibold), Theme::faint, cols[3]),
               Cell("WHERE IT LIVES (after code generation)", Fonts::mono(9.5f, Weight::Semibold), Theme::faint)},
              Row::Kind::Section, 18);
        return r;
    };
    auto symRow = [&](const Symbol& s, float indent = 0) {
        Color kindColor = s.kind == Symbol::Kind::Global ? Theme::ink
                          : s.kind == Symbol::Kind::Local ? Theme::green
                          : s.kind == Symbol::Kind::Param ? Theme::amber
                          : s.kind == Symbol::Kind::Function ? Theme::heading
                                                             : Theme::text;
        std::string name = s.uniqueName == s.name ? s.name : cat(s.name, "  (", s.uniqueName, ")");
        std::string loc = s.location;
        if (s.kind == Symbol::Kind::Function) {
            auto a = c->executable ? c->executable->address(s.name) : std::nullopt;
            loc = a ? "code at " + hexAddr(*a) : (s.isDefined ? "" : "declared only");
        } else if (s.kind == Symbol::Kind::Global && c->executable) {
            if (auto a = c->executable->address(s.uniqueName)) loc = ".data at " + hexAddr(*a);
        }
        Row r({Cell(name, Fonts::mono(12, Weight::Semibold), Theme::text, cols[0]),
               Cell(AttrString(symbolKindName(s.kind), Fonts::small, kindColor), cols[1], Align::Left, kindColor.alpha(0.1f)),
               Cell(s.type.description(), Fonts::mono(11.5f), Theme::type, cols[2]),
               Cell(s.kind == Symbol::Kind::Function ? "" : cat(s.type.size(), " B"), Fonts::mono(11), Theme::secondary, cols[3]),
               Cell(loc, Fonts::mono(11), hasPrefix(loc, "register") ? Theme::green : hasPrefix(loc, "optimised") ? Theme::faint : Theme::text)});
        r.line = s.line;
        r.indent = indent;
        r.height = 21;
        if (s.addressTaken) r.tooltip = cat("Its address is taken (&", s.name, "), so it must live in memory.");
        return r;
    };
    rows.push_back(Row::header("File scope", "globals and functions — visible everywhere after their declaration"));
    rows.push_back(headerRow());
    if (!sema.scopes.empty())
        for (auto& s : sema.scopes.front().symbols) rows.push_back(symRow(*s));
    for (auto& fd : prog.functions()) {
        rows.push_back(Row::spacer(6));
        std::string callNote = fd->hasCalls ? "makes calls" : "leaf function (makes no calls)";
        rows.push_back(Row::header("function " + fd->name, cat((int)fd->locals.size(), " parameters and locals · ", callNote)));
        rows.push_back(headerRow());
        // Order by scope depth, then declaration.
        for (auto& s : fd->locals) rows.push_back(symRow(*s, std::max(0, s->depth - 1) * 14.0f));
        if (fd->locals.empty()) rows.push_back(Row({Cell("(no parameters or locals)", Fonts::small, Theme::faint)}));
    }
    std::vector<StructDefP> structs;
    for (auto& s : prog.structs)
        if (s->isComplete) structs.push_back(s);
    if (!structs.empty()) {
        rows.push_back(Row::spacer(6));
        rows.push_back(Row::header("Struct layouts", "fields are aligned to their size, so the compiler may insert padding"));
        for (auto& st : structs) {
            Row hr({Cell("struct " + st->name, Fonts::mono(12, Weight::Semibold), Theme::purple, 260.0f),
                    Cell(cat(st->size, " bytes, aligned to ", st->align), Fonts::small, Theme::secondary)},
                   Row::Kind::Section, 22);
            rows.push_back(hr);
            int last = 0;
            for (auto& f : st->fields) {
                if (f.offset > last)
                    rows.push_back(Row({Cell("", Fonts::code, Theme::text, 20.0f), Cell("(padding)", Fonts::mono(11), Theme::faint, 170.0f),
                                        Cell(cat("offset ", last, "–", f.offset - 1), Fonts::mono(11), Theme::faint)}));
                rows.push_back(Row({Cell("", Fonts::code, Theme::text, 20.0f), Cell(f.name, Fonts::mono(11.5f, Weight::Semibold), Theme::text, 170.0f),
                                    Cell(f.type.description(), Fonts::mono(11), Theme::type, 170.0f),
                                    Cell(cat("offset ", f.offset, ", ", f.type.size(), " B"), Fonts::mono(11), Theme::secondary)}));
                last = f.offset + f.type.size();
            }
            if (st->size > last)
                rows.push_back(Row({Cell("", Fonts::code, Theme::text, 20.0f), Cell("(tail padding)", Fonts::mono(11), Theme::faint, 170.0f),
                                    Cell(cat("offset ", last, "–", st->size - 1), Fonts::mono(11), Theme::faint)}));
        }
    }
    if (!sema.libraryUsed.empty()) {
        rows.push_back(Row::spacer(6));
        rows.push_back(Row::header("Library functions called", "built in: no #include needed; the linker pulls in their code"));
        for (auto& n : sema.libraryUsed) {
            const Library::Function* f = Library::byName(n);
            std::string sig = f ? CType::function(f->returnType, f->params).spelled(n) : n;
            rows.push_back(Row({Cell(sig, Fonts::mono(11.5f), Theme::ink, 300.0f), Cell(f ? f->summary : "", Fonts::small, Theme::secondary)}));
        }
    }
    if (!sema.strings.empty()) {
        rows.push_back(Row::spacer(6));
        rows.push_back(Row::header("String literals", "stored once each in the .data section, ending with a zero byte"));
        for (auto& s : sema.strings)
            rows.push_back(Row({Cell(s.label, Fonts::mono(11.5f, Weight::Semibold), Theme::label, 80.0f),
                                Cell("\"" + CodeGen::escape(s.bytes) + "\"", Fonts::mono(11.5f), Theme::string, 360.0f),
                                Cell(cat((int)s.bytes.size() + 1, " bytes"), Fonts::mono(11), Theme::secondary)}));
    }
    panel.set(rows);
}

}  // namespace ui
