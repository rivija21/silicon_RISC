// A multi-line text view for code: editing, selection, clipboard, undo,
// syntax colours as styled runs, and per-line layout with DirectWrite.
// Lives inside a ScrollView as its document view.
#pragma once

#include <chrono>

#include "Theme.h"
#include "View.h"

namespace ui {

class TextView : public View {
public:
    TextView();

    // Content.
    void setText(const std::wstring& s);
    const std::wstring& text() const { return text_; }
    bool editable = true;
    Font font = Fonts::code;
    Color textColor = Theme::text;
    /// Extra space below every line.
    float lineSpacing = 0;
    float tabWidth = 28;
    Size inset{6, 8};
    Color selectionColor = Color::hex(0xD8E4F5);
    Color caretColor = Theme::ink;
    Color background = Theme::panel;

    struct Style {
        int start = 0, length = 0;
        Color color;
        Weight weight = Weight::Regular;
        bool italic = false;
    };
    void setStyles(std::vector<Style> styles);
    struct Underline {
        int start = 0, length = 0;
        Color color;
    };
    void setUnderlines(std::vector<Underline> u);

    std::function<void()> onTextChange;
    /// The user moved the caret or changed the selection.
    std::function<void()> onSelectionChange;

    // Selection (UTF-16 offsets).
    int selectionStart() const { return std::min(anchor_, caret_); }
    int selectionEnd() const { return std::max(anchor_, caret_); }
    int caret() const { return caret_; }
    void setSelection(int start, int end);
    void scrollRangeToVisible(int start, int end);
    void scrollToEnd();

    // Geometry (in this view's coordinates).
    float lineHeight() const;
    int lineCount() const { return (int)lineStarts_.size(); }
    int lineOf(int offset) const;
    int lineStart(int line0) const { return lineStarts_[line0]; }
    int lineEnd(int line0) const;
    /// The band of a 1-based line, from x = 0 to the right edge.
    Rect rectOfLine(int line1) const;
    /// Resizes to fit the text (at least as big as the visible area).
    void updateSize();

    void draw(Gfx& g, const Rect& dirty) override;
    Cursor cursor(Point) override { return Cursor::IBeam; }
    bool acceptsFocus() const override { return true; }
    void focusChanged(bool focused) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDragged(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override {}
    void mouseMoved(const MouseEvent& e) override {}
    void keyDown(const KeyEvent& e) override;
    void charInput(wchar_t c) override;
    bool editCommand(const std::string& cmd) override;
    std::optional<Rect> caretRect() const override;
    bool focused() const { return focused_; }

protected:
    /// Drawn under the selection and the text.
    virtual void drawBackground(Gfx& g, const Rect& dirty) {}
    virtual void insertNewline();
    virtual void insertTab();
    /// Replaces the selection with text, as one undoable edit.
    void insertText(const std::wstring& s, bool typing = false);

private:
    std::wstring text_;
    std::vector<int> lineStarts_{0};
    std::vector<Style> styles_;
    std::vector<Underline> underlines_;
    mutable std::vector<Com<IDWriteTextLayout>> layouts_;
    int anchor_ = 0, caret_ = 0;
    float preferredX_ = -1;
    bool focused_ = false;
    wchar_t pendingHigh_ = 0;
    int dragMode_ = 0;   // 1 chars, 2 words, 3 lines
    int dragOrigin_ = 0, dragOriginEnd_ = 0;

    struct Edit {
        int pos;
        std::wstring removed, inserted;
        int selBeforeStart, selBeforeEnd;
        bool typing;
        std::chrono::steady_clock::time_point when;
    };
    std::vector<Edit> undo_, redo_;
    bool coalesce_ = false;

    void rebuildLines();
    void invalidateLayouts();
    IDWriteTextLayout* lineLayout(int line0) const;
    float charWidth() const;
    float xOf(int offset) const;
    int offsetAt(Point p) const;
    void replace(int start, int end, const std::wstring& s, bool typing);
    void applyRaw(int start, int end, const std::wstring& s);
    void moveCaret(int to, bool extend);
    void selectionChanged();
    int wordStart(int k) const;
    int wordEnd(int k) const;
    void copy();
    void paste();
    void doUndo();
    void doRedo();
    void ensureCaretVisible();
};

}  // namespace ui
