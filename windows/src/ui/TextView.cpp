#include "TextView.h"

#include <cwctype>

namespace ui {

TextView::TextView() { rebuildLines(); }

static std::wstring withoutCR(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (size_t k = 0; k < s.size(); k++) {
        if (s[k] == L'\r') {
            if (k + 1 < s.size() && s[k + 1] == L'\n') continue;
            out += L'\n';
            continue;
        }
        out += s[k];
    }
    return out;
}

void TextView::setText(const std::wstring& s) {
    text_ = withoutCR(s);
    rebuildLines();
    invalidateLayouts();
    undo_.clear();
    redo_.clear();
    coalesce_ = false;
    anchor_ = std::min(anchor_, (int)text_.size());
    caret_ = std::min(caret_, (int)text_.size());
    updateSize();
    setNeedsDisplay();
}

void TextView::setStyles(std::vector<Style> styles) {
    styles_ = std::move(styles);
    std::sort(styles_.begin(), styles_.end(), [](const Style& a, const Style& b) { return a.start < b.start; });
    invalidateLayouts();
    setNeedsDisplay();
}

void TextView::setUnderlines(std::vector<Underline> u) {
    underlines_ = std::move(u);
    setNeedsDisplay();
}

void TextView::rebuildLines() {
    lineStarts_.assign(1, 0);
    for (int k = 0; k < (int)text_.size(); k++)
        if (text_[k] == L'\n') lineStarts_.push_back(k + 1);
}

void TextView::invalidateLayouts() {
    layouts_.clear();
    layouts_.resize(lineStarts_.size());
}

int TextView::lineEnd(int line0) const {
    return line0 + 1 < (int)lineStarts_.size() ? lineStarts_[line0 + 1] - 1 : (int)text_.size();
}

int TextView::lineOf(int offset) const {
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    return std::max(0, (int)(it - lineStarts_.begin()) - 1);
}

float TextView::lineHeight() const { return TextEngine::get().lineHeight(font) + lineSpacing; }

float TextView::charWidth() const { return TextEngine::get().measure(L"0", font).w; }

IDWriteTextLayout* TextView::lineLayout(int i) const {
    if ((int)layouts_.size() != lineCount()) layouts_.resize(lineCount());
    if (layouts_[i]) return layouts_[i].get();
    TextEngine& te = TextEngine::get();
    int s = lineStarts_[i], e = lineEnd(i);
    std::wstring line = text_.substr(s, e - s);
    Com<IDWriteTextLayout> l;
    te.factory()->CreateTextLayout(line.c_str(), (UINT32)line.size(), te.format(font), 1e6f, 1e6f, l.put());
    if (!l) return nullptr;
    l->SetIncrementalTabStop(tabWidth);
    for (auto& st : styles_) {
        int a = std::max(st.start, s), b = std::min(st.start + st.length, e);
        if (st.start >= e) break;
        if (b <= a) continue;
        DWRITE_TEXT_RANGE r{(UINT32)(a - s), (UINT32)(b - a)};
        l->SetDrawingEffect(te.colorEffect(st.color), r);
        if (st.weight != Weight::Regular)
            l->SetFontWeight(st.weight == Weight::Bold ? DWRITE_FONT_WEIGHT_BOLD
                             : st.weight == Weight::Semibold ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                                             : DWRITE_FONT_WEIGHT_MEDIUM,
                             r);
        if (st.italic) l->SetFontStyle(DWRITE_FONT_STYLE_ITALIC, r);
    }
    layouts_[i] = l;
    return l.get();
}

Rect TextView::rectOfLine(int line1) const {
    float lh = lineHeight();
    return {0, inset.h + (line1 - 1) * lh, std::max(width(), 2000.0f), lh};
}

void TextView::updateSize() {
    int maxCols = 0;
    for (int i = 0; i < lineCount(); i++) {
        int cols = 0;
        for (int k = lineStarts_[i]; k < lineEnd(i); k++) cols = text_[k] == L'\t' ? (cols / 4 + 1) * 4 : cols + 1;
        maxCols = std::max(maxCols, cols);
    }
    float w = inset.w * 2 + maxCols * charWidth() + 24;
    float h = inset.h * 2 + lineCount() * lineHeight() + 4;
    Size vis = frame().size();
    if (ScrollView* s = enclosingScrollView()) vis = s->contentSize();
    Rect f = frame();
    f.w = std::max(w, vis.w);
    f.h = std::max(h, vis.h);
    setFrame(f);
    if (ScrollView* s = enclosingScrollView()) s->clampOffset();
}

float TextView::xOf(int offset) const {
    int i = lineOf(offset);
    IDWriteTextLayout* l = lineLayout(i);
    if (!l) return inset.w;
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m{};
    l->HitTestTextPosition((UINT32)(offset - lineStarts_[i]), FALSE, &x, &y, &m);
    return inset.w + x;
}

int TextView::offsetAt(Point p) const {
    float lh = lineHeight();
    int i = (int)std::floor((p.y - inset.h) / lh);
    if (i < 0) return 0;
    if (i >= lineCount()) return (int)text_.size();
    IDWriteTextLayout* l = lineLayout(i);
    if (!l) return lineStarts_[i];
    BOOL trailing = FALSE, inside = FALSE;
    DWRITE_HIT_TEST_METRICS m{};
    l->HitTestPoint(p.x - inset.w, lh / 2, &trailing, &inside, &m);
    int k = (int)m.textPosition + (trailing ? (int)m.length : 0);
    return std::min(lineStarts_[i] + k, lineEnd(i));
}

std::optional<Rect> TextView::caretRect() const {
    int i = lineOf(caret_);
    Rect r{xOf(caret_), inset.h + i * lineHeight(), 2, TextEngine::get().lineHeight(font)};
    return convertRectToWindow(r);
}

// MARK: Drawing

void TextView::draw(Gfx& g, const Rect& dirty) {
    g.fillRect(dirty, background);
    drawBackground(g, dirty);
    float lh = lineHeight();
    int first = std::max(0, (int)std::floor((dirty.minY() - inset.h) / lh) - 1);
    int last = std::min(lineCount() - 1, (int)std::ceil((dirty.maxY() - inset.h) / lh) + 1);
    int ss = selectionStart(), se = selectionEnd();
    for (int i = first; i <= last; i++) {
        float y = inset.h + i * lh;
        IDWriteTextLayout* l = lineLayout(i);
        if (!l) continue;
        int s = lineStarts_[i], e = lineEnd(i);
        // Selection.
        if (se > ss && ss <= e && se >= s) {
            int a = std::max(ss, s), b = std::min(se, e);
            if (b > a) {
                UINT32 count = 0;
                DWRITE_HIT_TEST_METRICS m[16];
                if (SUCCEEDED(l->HitTestTextRange(a - s, b - a, 0, 0, m, 16, &count)))
                    for (UINT32 k = 0; k < count; k++) g.fillRect({inset.w + m[k].left, y, m[k].width, lh}, selectionColor);
            }
            if (se > e && i + 1 < lineCount()) {
                float x = xOf(e);
                g.fillRect({x, y, std::max(6.0f, width() - x), lh}, selectionColor);
            }
        }
        g.drawLayout(l, {inset.w, y}, textColor);
        // Error underlines: a dotted line under the range.
        for (auto& u : underlines_) {
            int a = std::max(u.start, s), b = std::min(u.start + u.length, e);
            if (b <= a) continue;
            UINT32 count = 0;
            DWRITE_HIT_TEST_METRICS m[16];
            if (SUCCEEDED(l->HitTestTextRange(a - s, b - a, 0, 0, m, 16, &count))) {
                float base = y + TextEngine::get().lineHeight(font) - 1.5f;
                for (UINT32 k = 0; k < count; k++)
                    g.line({inset.w + m[k].left, base}, {inset.w + m[k].left + std::max(4.0f, m[k].width), base}, u.color, 2, {2, 2});
            }
        }
    }
    if (focused_ && editable && ss == se) {
        int i = lineOf(caret_);
        float x = xOf(caret_);
        g.fillRect({std::floor(x) - 0.5f, inset.h + i * lh, 1.5f, TextEngine::get().lineHeight(font)}, caretColor);
    }
}

void TextView::focusChanged(bool f) {
    focused_ = f;
    setNeedsDisplay();
}

// MARK: Selection

void TextView::setSelection(int start, int end) {
    int n = (int)text_.size();
    anchor_ = std::max(0, std::min(start, n));
    caret_ = std::max(0, std::min(end, n));
    preferredX_ = -1;
    coalesce_ = false;
    setNeedsDisplay();
}

void TextView::selectionChanged() {
    setNeedsDisplay();
    if (Host* h = host()) h->invalidate();
    if (onSelectionChange) onSelectionChange();
}

void TextView::moveCaret(int to, bool extend) {
    to = std::max(0, std::min(to, (int)text_.size()));
    caret_ = to;
    if (!extend) anchor_ = to;
    coalesce_ = false;
    ensureCaretVisible();
    selectionChanged();
}

void TextView::ensureCaretVisible() {
    int i = lineOf(caret_);
    float x = xOf(caret_);
    scrollToVisible({std::max(0.0f, x - 30), inset.h + i * lineHeight() - 2, 60, lineHeight() + 4});
}

void TextView::scrollRangeToVisible(int start, int end) {
    int a = lineOf(start), b = lineOf(end);
    float lh = lineHeight();
    Rect r{std::max(0.0f, xOf(start) - 30), inset.h + a * lh - 4, 60, (b - a + 1) * lh + 8};
    scrollToVisible(r);
}

void TextView::scrollToEnd() { scrollToVisible({0, height() - 2, 1, 2}); }

static bool isWordChar(wchar_t c) { return std::iswalnum(c) || c == L'_'; }

int TextView::wordStart(int k) const {
    while (k > 0 && isWordChar(text_[k - 1])) k--;
    return k;
}

int TextView::wordEnd(int k) const {
    while (k < (int)text_.size() && isWordChar(text_[k])) k++;
    return k;
}

// MARK: Mouse

void TextView::mouseDown(const MouseEvent& e) {
    if (Host* h = host()) h->setFocus(this);
    int k = offsetAt(localPoint(e));
    preferredX_ = -1;
    if (e.clickCount >= 3) {
        int i = lineOf(k);
        dragMode_ = 3;
        dragOrigin_ = lineStarts_[i];
        dragOriginEnd_ = i + 1 < lineCount() ? lineStarts_[i + 1] : (int)text_.size();
        anchor_ = dragOrigin_;
        caret_ = dragOriginEnd_;
    } else if (e.clickCount == 2) {
        dragMode_ = 2;
        int a = wordStart(k), b = wordEnd(k);
        if (a == b && k < (int)text_.size()) b = k + 1;
        dragOrigin_ = a;
        dragOriginEnd_ = b;
        anchor_ = a;
        caret_ = b;
    } else {
        dragMode_ = 1;
        caret_ = k;
        if (!e.shift) anchor_ = k;
        dragOrigin_ = anchor_;
    }
    coalesce_ = false;
    selectionChanged();
}

void TextView::mouseDragged(const MouseEvent& e) {
    Point p = localPoint(e);
    int k = offsetAt(p);
    if (dragMode_ == 2) {
        int a = wordStart(k), b = wordEnd(k);
        if (k < dragOrigin_) {
            anchor_ = dragOriginEnd_;
            caret_ = a;
        } else {
            anchor_ = dragOrigin_;
            caret_ = std::max(b, dragOriginEnd_);
        }
    } else if (dragMode_ == 3) {
        int i = lineOf(k);
        if (k < dragOrigin_) {
            anchor_ = dragOriginEnd_;
            caret_ = lineStarts_[i];
        } else {
            anchor_ = dragOrigin_;
            caret_ = i + 1 < lineCount() ? lineStarts_[i + 1] : (int)text_.size();
        }
    } else {
        caret_ = k;
    }
    scrollToVisible({p.x - 10, p.y - 10, 20, 20});
    selectionChanged();
}

// MARK: Keyboard

void TextView::keyDown(const KeyEvent& e) {
    int n = (int)text_.size();
    auto prevChar = [&](int k) {
        if (k >= 2 && text_[k - 1] >= 0xDC00 && text_[k - 1] <= 0xDFFF && text_[k - 2] >= 0xD800 && text_[k - 2] <= 0xDBFF) return k - 2;
        return std::max(0, k - 1);
    };
    auto nextChar = [&](int k) {
        if (k + 1 < n && text_[k] >= 0xD800 && text_[k] <= 0xDBFF && text_[k + 1] >= 0xDC00 && text_[k + 1] <= 0xDFFF) return k + 2;
        return std::min(n, k + 1);
    };
    auto prevWord = [&](int k) {
        while (k > 0 && !isWordChar(text_[k - 1])) k--;
        return wordStart(k);
    };
    auto nextWord = [&](int k) {
        while (k < n && !isWordChar(text_[k])) k++;
        return wordEnd(k);
    };
    int ss = selectionStart(), se = selectionEnd();
    switch (e.vk) {
    case VK_LEFT:
        if (ss != se && !e.shift) moveCaret(ss, false);
        else moveCaret(e.ctrl ? prevWord(caret_) : prevChar(caret_), e.shift);
        preferredX_ = -1;
        return;
    case VK_RIGHT:
        if (ss != se && !e.shift) moveCaret(se, false);
        else moveCaret(e.ctrl ? nextWord(caret_) : nextChar(caret_), e.shift);
        preferredX_ = -1;
        return;
    case VK_UP:
    case VK_DOWN:
    case VK_PRIOR:
    case VK_NEXT: {
        float lh = lineHeight();
        int step = 1;
        if (e.vk == VK_PRIOR || e.vk == VK_NEXT) {
            float vis = enclosingScrollView() ? enclosingScrollView()->contentSize().h : height();
            step = std::max(1, (int)(vis / lh) - 1);
        }
        int dir = (e.vk == VK_UP || e.vk == VK_PRIOR) ? -1 : 1;
        float px = preferredX_ >= 0 ? preferredX_ : xOf(caret_);
        int i = lineOf(caret_) + dir * step;
        int to;
        if (i < 0) to = 0;
        else if (i >= lineCount()) to = n;
        else to = offsetAt({px, inset.h + i * lh + lh / 2});
        moveCaret(to, e.shift);
        preferredX_ = px;
        return;
    }
    case VK_HOME: {
        if (e.ctrl) {
            moveCaret(0, e.shift);
            return;
        }
        int i = lineOf(caret_);
        int s = lineStarts_[i], k = s;
        while (k < lineEnd(i) && (text_[k] == L' ' || text_[k] == L'\t')) k++;
        moveCaret(caret_ == k ? s : k, e.shift);
        return;
    }
    case VK_END:
        moveCaret(e.ctrl ? n : lineEnd(lineOf(caret_)), e.shift);
        return;
    case VK_BACK:
        if (!editable) return;
        if (ss != se) replace(ss, se, L"", false);
        else if (caret_ > 0) replace(e.ctrl ? prevWord(caret_) : prevChar(caret_), caret_, L"", false);
        return;
    case VK_DELETE:
        if (!editable) return;
        if (ss != se) replace(ss, se, L"", false);
        else if (caret_ < n) replace(caret_, e.ctrl ? nextWord(caret_) : nextChar(caret_), L"", false);
        return;
    case VK_RETURN:
        if (editable) insertNewline();
        return;
    case VK_TAB:
        if (!editable) {
            View::keyDown(e);
            return;
        }
        if (e.shift) {
            // Unindent the selected lines by up to four spaces.
            int a = lineOf(ss), b = lineOf(std::max(ss, se - (se > ss ? 1 : 0)));
            std::wstring out;
            int start = lineStarts_[a], end = lineEnd(b);
            for (int i = a; i <= b; i++) {
                std::wstring l = text_.substr(lineStarts_[i], lineEnd(i) - lineStarts_[i]);
                int k = 0;
                while (k < 4 && k < (int)l.size() && l[k] == L' ') k++;
                out += l.substr(k);
                if (i < b) out += L'\n';
            }
            if (out != text_.substr(start, end - start)) {
                replace(start, end, out, false);
                setSelection(start, start + (int)out.size());
            }
        } else {
            insertTab();
        }
        return;
    default: View::keyDown(e); return;
    }
}

void TextView::charInput(wchar_t c) {
    if (!editable) return;
    if (c >= 0xD800 && c <= 0xDBFF) {
        pendingHigh_ = c;
        return;
    }
    if (c >= 0xDC00 && c <= 0xDFFF) {
        if (pendingHigh_) {
            std::wstring s{pendingHigh_, c};
            pendingHigh_ = 0;
            insertText(s, true);
        }
        return;
    }
    pendingHigh_ = 0;
    if (c < 0x20 || c == 0x7F) return;
    insertText(std::wstring(1, c), true);
}

void TextView::insertNewline() { insertText(L"\n"); }
void TextView::insertTab() { insertText(L"    ", true); }
void TextView::insertText(const std::wstring& s, bool typing) { replace(selectionStart(), selectionEnd(), withoutCR(s), typing); }

// MARK: Editing

void TextView::applyRaw(int start, int end, const std::wstring& s) {
    text_.replace(start, end - start, s);
    rebuildLines();
    invalidateLayouts();
    updateSize();
    setNeedsDisplay();
}

void TextView::replace(int start, int end, const std::wstring& s, bool typing) {
    if (start == end && s.empty()) return;
    auto now = std::chrono::steady_clock::now();
    std::wstring removed = text_.substr(start, end - start);
    bool merged = false;
    if (typing && coalesce_ && !undo_.empty() && removed.empty() && s != L"\n") {
        Edit& last = undo_.back();
        if (last.typing && last.pos + (int)last.inserted.size() == start && now - last.when < std::chrono::seconds(3)) {
            last.inserted += s;
            last.when = now;
            merged = true;
        }
    }
    if (!merged) undo_.push_back(Edit{start, removed, s, anchor_, caret_, typing, now});
    if (undo_.size() > 500) undo_.erase(undo_.begin());
    redo_.clear();
    applyRaw(start, end, s);
    anchor_ = caret_ = start + (int)s.size();
    coalesce_ = typing;
    preferredX_ = -1;
    ensureCaretVisible();
    if (onTextChange) onTextChange();
    selectionChanged();
}

void TextView::doUndo() {
    if (undo_.empty()) return;
    Edit e = undo_.back();
    undo_.pop_back();
    applyRaw(e.pos, e.pos + (int)e.inserted.size(), e.removed);
    anchor_ = std::min(e.selBeforeStart, (int)text_.size());
    caret_ = std::min(e.selBeforeEnd, (int)text_.size());
    redo_.push_back(e);
    coalesce_ = false;
    ensureCaretVisible();
    if (onTextChange) onTextChange();
    selectionChanged();
}

void TextView::doRedo() {
    if (redo_.empty()) return;
    Edit e = redo_.back();
    redo_.pop_back();
    applyRaw(e.pos, e.pos + (int)e.removed.size(), e.inserted);
    anchor_ = caret_ = e.pos + (int)e.inserted.size();
    undo_.push_back(e);
    coalesce_ = false;
    ensureCaretVisible();
    if (onTextChange) onTextChange();
    selectionChanged();
}

void TextView::copy() {
    int a = selectionStart(), b = selectionEnd();
    if (a == b) return;
    std::wstring s;
    for (wchar_t c : text_.substr(a, b - a)) {
        if (c == L'\n') s += L'\r';
        s += c;
    }
    Host* h = host();
    if (!OpenClipboard(h ? h->hwnd() : nullptr)) return;
    EmptyClipboard();
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (mem) {
        memcpy(GlobalLock(mem), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(mem);
        if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
    }
    CloseClipboard();
}

void TextView::paste() {
    Host* h = host();
    if (!OpenClipboard(h ? h->hwnd() : nullptr)) return;
    std::wstring s;
    if (HANDLE d = GetClipboardData(CF_UNICODETEXT)) {
        if (auto* p = (const wchar_t*)GlobalLock(d)) {
            s = p;
            GlobalUnlock(d);
        }
    }
    CloseClipboard();
    if (!s.empty()) insertText(s);
}

bool TextView::editCommand(const std::string& cmd) {
    if (cmd == "copy") {
        copy();
        return true;
    }
    if (cmd == "selectAll") {
        anchor_ = 0;
        caret_ = (int)text_.size();
        selectionChanged();
        return true;
    }
    if (!editable) return false;
    if (cmd == "cut") {
        copy();
        if (selectionStart() != selectionEnd()) replace(selectionStart(), selectionEnd(), L"", false);
        return true;
    }
    if (cmd == "paste") {
        paste();
        return true;
    }
    if (cmd == "undo") {
        doUndo();
        return true;
    }
    if (cmd == "redo") {
        doRedo();
        return true;
    }
    return false;
}

}  // namespace ui
