#include <cwctype>

#include "Views.h"

namespace ui {

using namespace sp;

// MARK: - Find bar

void FindField::keyDown(const KeyEvent& e) {
    if (e.vk == VK_ESCAPE) {
        if (onEscape) onEscape();
        return;
    }
    if (e.vk == VK_RETURN) {
        if (onEnter) onEnter(e.shift ? -1 : 1);
        return;
    }
    if (e.vk == VK_UP || e.vk == VK_DOWN || e.vk == VK_PRIOR || e.vk == VK_NEXT) return;
    TextView::keyDown(e);
}

FindBar::FindBar() {
    field.font = Fonts::ui(12);
    field.inset = {6, 3};
    field.background = Theme::panel;
    for (View* v : std::initializer_list<View*>{&field, &prev, &next, &close, &status}) addSubview(v);
    prev.tooltip = "Previous match (Shift+Enter)";
    next.tooltip = "Next match (Enter)";
    close.tooltip = "Close (Esc)";
    prev.action = [this] {
        if (onFind) onFind(-1);
    };
    next.action = [this] {
        if (onFind) onFind(1);
    };
    close.action = [this] {
        if (onClose) onClose();
    };
    field.onEnter = [this](int d) {
        if (onFind) onFind(d);
    };
    field.onEscape = [this] {
        if (onClose) onClose();
    };
}

void FindBar::layout() {
    float h = height();
    close.setFrame({width() - 34, (h - 24) / 2, 26, 24});
    next.setFrame({width() - 64, (h - 24) / 2, 26, 24});
    prev.setFrame({width() - 94, (h - 24) / 2, 26, 24});
    status.setFrame({width() - 190, 0, 92, h});
    status.align = Align::Right;
    field.setFrame({12, (h - 22) / 2, std::max(60.0f, width() - 210), 22});
    field.updateSize();
}

void FindBar::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panelAlt);
    line({0, height() - 0.5f}, {width(), height() - 0.5f}, Theme::border);
    Rect f = field.frame().insetBy(-1, -1);
    fillRounded(f, 5, Theme::panel);
    strokeRounded(f, 5, field.focused() ? Theme::ink.alpha(0.6f) : Theme::borderStrong);
}

// MARK: - Editor text view

void EditorTextView::drawBackground(Gfx& g, const Rect& dirty) {
    if (!editor) return;
    // Lines with instructions in the pipeline get a soft band in the colour of the furthest stage.
    for (auto& [line, st] : editor->stages()) {
        auto r = editor->rectOfLine(line);
        if (!r || !r->intersects(dirty) || st.empty()) continue;
        int k = *std::max_element(st.begin(), st.end());
        g.fillRect({0, r->minY(), width(), r->h}, Theme::stageFill(k).alpha(0.55f));
    }
    if (auto l = editor->selectedLine()) {
        if (auto r = editor->rectOfLine(*l)) {
            g.fillRect({0, r->minY(), width(), r->h}, Theme::selection);
            g.fillRect({0, r->minY(), 3, r->h}, Theme::selectionStrong);
        }
    }
    for (auto& d : editor->errors()) {
        if (d.severity != Diagnostic::Severity::Error || d.line <= 0) continue;
        if (auto r = editor->rectOfLine(d.line)) g.fillRect({0, r->minY(), width(), r->h}, Theme::errorFill.alpha(0.6f));
    }
}

void EditorTextView::insertNewline() {
    // Keep the indentation of the current line, and indent after "{".
    int c = selectionStart();
    int ls = lineStart(lineOf(c));
    std::wstring lineText = text().substr(ls, c - ls);
    std::wstring indent;
    for (wchar_t ch : lineText) {
        if (ch == L' ' || ch == L'\t') indent += ch;
        else break;
    }
    std::wstring t = lineText;
    while (!t.empty() && (t.back() == L' ' || t.back() == L'\t')) t.pop_back();
    if (!t.empty() && t.back() == L'{') indent += L"    ";
    insertText(L"\n" + indent);
}

// MARK: - Line ruler

void LineRuler::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panelAlt);
    line({width() - 0.5f, 0}, {width() - 0.5f, height()}, Theme::border);
    if (!editor) return;
    CodeEditor& e = *editor;
    Rect visible = e.scroll.documentVisibleRect();
    float offset = -visible.y;
    std::set<int> errLines, warnLines;
    for (auto& d : e.errors()) {
        if (d.severity == Diagnostic::Severity::Error) errLines.insert(d.line);
        else if (d.severity == Diagnostic::Severity::Warning) warnLines.insert(d.line);
    }
    static const char* stageNames[] = {"F", "D", "X", "M", "W"};
    int count = std::max(1, e.textView.lineCount());
    float lh = e.textView.lineHeight();
    int first = std::max(1, (int)((visible.minY() - 20 - e.textView.inset.h) / lh));
    for (int n = first; n <= count; n++) {
        auto r = e.rectOfLine(n);
        if (!r) continue;
        if (r->maxY() < visible.minY() - 20) continue;
        if (r->minY() > visible.maxY() + 20) break;
        float y = r->minY() + offset;
        bool isSel = e.selectedLine() == n;
        std::string s = std::to_string(n);
        Font font = isSel ? Fonts::mono(10.5f, Weight::Bold) : Fonts::mono(10.5f);
        Size sz = textSize(s, font);
        drawText(s, Point(width() - 8 - sz.w, y + (r->h - sz.h) / 2), font, isSel ? Theme::text : Theme::faint);
        if (errLines.count(n)) fillRounded({4, y + r->h / 2 - 4, 8, 8}, 4, Theme::crimson);
        else if (warnLines.count(n)) fillRounded({4, y + r->h / 2 - 4, 8, 8}, 4, Theme::amber);
        auto it = e.stages().find(n);
        if (it != e.stages().end()) {
            std::set<int> ks(it->second.begin(), it->second.end());
            float x = 3;
            for (int k : ks) {
                Rect b{x, y + (r->h - 12) / 2, 12, 12};
                fillRounded(b, 3, Theme::stage[k]);
                drawText(stageNames[k], b, Fonts::mono(8.5f, Weight::Bold), Color::white(), Align::Center);
                x += 7;
            }
        }
    }
}

void LineRuler::mouseDown(const MouseEvent& ev) {
    if (!editor) return;
    CodeEditor& e = *editor;
    Point p = localPoint(ev);
    float offset = -e.scroll.documentVisibleRect().y;
    for (int n = 1; n <= std::max(1, e.textView.lineCount()); n++) {
        auto r = e.rectOfLine(n);
        if (r && p.y >= r->minY() + offset && p.y < r->maxY() + offset) {
            if (e.onSelectLine) e.onSelectLine(n);
            return;
        }
    }
}

// MARK: - Code editor

CodeEditor::CodeEditor() {
    textView.editor = this;
    textView.font = Fonts::code;
    textView.textColor = Theme::text;
    textView.background = Theme::panel;
    textView.caretColor = Theme::ink;
    textView.selectionColor = Color::hex(0xD8E4F5);
    textView.inset = {6, 8};
    textView.lineSpacing = 2.5f;
    textView.tabWidth = 28;
    scroll.setDocumentView(&textView);
    scroll.hasVerticalScroller = true;
    scroll.hasHorizontalScroller = true;
    scroll.background = Theme::panel;
    ruler.editor = this;
    addSubview(&scroll);
    addSubview(&ruler);
    addSubview(&findBar);
    findBar.setHidden(true);
    scroll.onScroll = [this] { ruler.setNeedsDisplay(); };
    textView.onTextChange = [this] {
        if (suppress_) return;
        highlight();
        if (onChange) onChange(text());
    };
    textView.onSelectionChange = [this] {
        if (suppress_ || !textView.focused()) return;
        if (onSelectLine) onSelectLine(textView.lineOf(textView.caret()) + 1);
    };
    findBar.onFind = [this](int d) { find(d); };
    findBar.onClose = [this] {
        findVisible_ = false;
        findBar.setHidden(true);
        setNeedsLayout();
        if (Host* h = host()) h->setFocus(&textView);
    };
}

void CodeEditor::layout() {
    float gw = 58;
    float top = findVisible_ ? 34 : 0;
    findBar.setFrame({0, 0, width(), 34});
    ruler.setFrame({0, top, gw, height() - top});
    scroll.setFrame({gw, top, width() - gw, height() - top});
    textView.updateSize();
    ruler.setNeedsDisplay();
}

std::string CodeEditor::text() const { return narrow(textView.text()); }

void CodeEditor::setText(const std::string& s, bool assembly) {
    isAssembly_ = assembly;
    std::wstring w = widen(s);
    if (w == textView.text()) {
        highlight();
        return;
    }
    suppress_ = true;
    textView.setText(w);
    textView.setSelection(0, 0);
    suppress_ = false;
    highlight();
    scroll.scrollTo({0, 0});
}

void CodeEditor::highlight() {
    const std::wstring& s = textView.text();
    std::vector<TextView::Style> styles;
    auto color = [&](SourceRange r, const Color& c, Weight w = Weight::Regular, bool italic = false) {
        if (!r.isValid() || r.end > (int)s.size() || r.length() <= 0) return;
        styles.push_back({r.start, r.length(), c, w, italic});
    };
    if (isAssembly_) {
        int start = 0;
        for (int i = 0; i <= (int)s.size(); i++) {
            if (i == (int)s.size() || s[i] == L'\n') {
                AttrString a = highlightAsm(narrow(s.substr(start, i - start)));
                for (auto& run : a.runs())
                    if (run.start + run.length <= i - start) styles.push_back({start + run.start, run.length, run.color});
                start = i + 1;
            }
        }
    } else {
        LexResult lx = lex(narrow(s));
        for (auto& t : lx.tokens) {
            if (t.fromMacro) continue;
            switch (t.kind) {
            case TokenKind::Keyword: {
                static const std::set<std::string> types = {"int", "char", "void", "unsigned", "signed", "long", "struct", "const"};
                color(t.range, types.count(t.text) ? Theme::type : Theme::keyword, Weight::Semibold);
                break;
            }
            case TokenKind::Number: case TokenKind::Character: color(t.range, Theme::number); break;
            case TokenKind::String: color(t.range, Theme::string); break;
            case TokenKind::Punct: color(t.range, Theme::punct); break;
            case TokenKind::Identifier:
                if (lx.macros.count(t.text)) color(t.range, Theme::directive);
                else if (Library::byName(t.text) || t.text == "printf") color(t.range, Theme::ink);
                break;
            default: break;
            }
        }
        for (auto& tr : lx.trivia) {
            bool isComment = tr.kind == Trivia::Kind::Comment;
            color(tr.range, isComment ? Theme::comment : Theme::directive, Weight::Regular, isComment);
        }
    }
    textView.setStyles(std::move(styles));
    // Error underlines.
    std::vector<TextView::Underline> unders;
    for (auto& d : errors_) {
        if (d.severity != Diagnostic::Severity::Error || !d.range.isValid() || d.range.end > (int)s.size()) continue;
        int len = std::max(1, d.range.length());
        if (d.range.start + len > (int)s.size()) len = std::max(0, (int)s.size() - d.range.start);
        unders.push_back({d.range.start, len, Theme::crimson});
    }
    textView.setUnderlines(std::move(unders));
}

void CodeEditor::refreshMarks() {
    textView.setNeedsDisplay();
    ruler.setNeedsDisplay();
}

void CodeEditor::setErrors(std::vector<Diagnostic> d) {
    errors_ = std::move(d);
    highlight();
    refreshMarks();
}

void CodeEditor::setStages(const std::map<int, std::vector<int>>& s) {
    if (s == stages_) return;
    stages_ = s;
    refreshMarks();
}

void CodeEditor::setSelectedLine(std::optional<int> l) {
    if (l == selectedLine_) return;
    selectedLine_ = l;
    refreshMarks();
}

std::optional<Rect> CodeEditor::rectOfLine(int line) const {
    if (line < 1 || line > textView.lineCount()) return std::nullopt;
    return textView.rectOfLine(line);
}

void CodeEditor::reveal(int line) {
    auto r = rectOfLine(line);
    if (!r) return;
    Rect visible = scroll.documentVisibleRect();
    if (r->minY() < visible.minY() + 20 || r->maxY() > visible.maxY() - 20) {
        scroll.scrollTo({visible.x, std::max(0.0f, r->midY() - visible.h / 2)});
    }
}

void CodeEditor::select(SourceRange range) {
    if (!range.isValid() || range.end > (int)textView.text().size()) return;
    suppress_ = true;
    textView.setSelection(range.start, range.end);
    suppress_ = false;
    textView.scrollRangeToVisible(range.start, range.end);
}

void CodeEditor::showFind() {
    findVisible_ = true;
    findBar.setHidden(false);
    setNeedsLayout();
    layoutSubtreeIfNeeded();
    if (Host* h = host()) h->setFocus(&findBar.field);
    findBar.field.setSelection(0, (int)findBar.field.text().size());
}

void CodeEditor::find(int direction) {
    const std::wstring& q = findBar.field.text();
    const std::wstring& s = textView.text();
    if (q.empty()) {
        findBar.status.setText("");
        return;
    }
    auto lower = [](std::wstring x) {
        for (auto& c : x) c = (wchar_t)std::towlower(c);
        return x;
    };
    std::wstring ls = lower(s), lq = lower(q);
    std::vector<int> hits;
    for (size_t k = ls.find(lq); k != std::wstring::npos; k = ls.find(lq, k + 1)) hits.push_back((int)k);
    if (hits.empty()) {
        findBar.status.setText("not found");
        return;
    }
    int from = direction > 0 ? textView.selectionEnd() : textView.selectionStart();
    int pick = -1;
    if (direction > 0) {
        for (int k = 0; k < (int)hits.size(); k++)
            if (hits[k] >= from) {
                pick = k;
                break;
            }
        if (pick < 0) pick = 0;
    } else {
        for (int k = (int)hits.size() - 1; k >= 0; k--)
            if (hits[k] < from) {
                pick = k;
                break;
            }
        if (pick < 0) pick = (int)hits.size() - 1;
    }
    textView.setSelection(hits[pick], hits[pick] + (int)q.size());
    textView.scrollRangeToVisible(hits[pick], hits[pick] + (int)q.size());
    findBar.status.setText(cat(pick + 1, " of ", (int)hits.size()));
    textView.setNeedsDisplay();
}

bool CodeEditor::editCommand(const std::string& cmd) {
    if (cmd == "find") {
        showFind();
        return true;
    }
    if (cmd == "findNext") {
        find(1);
        return true;
    }
    return false;
}

}  // namespace ui
