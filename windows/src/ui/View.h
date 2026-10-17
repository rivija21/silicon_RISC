// A small retained view system in the spirit of AppKit's NSView: a tree of
// views with frames, custom drawing, layout, mouse and keyboard events, plus
// scroll views and split views. All views use a top-left origin.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "Gfx.h"

namespace ui {

class View;

struct MouseEvent {
    /// Location in window coordinates (DIPs).
    Point window;
    int clickCount = 1;
    bool shift = false, ctrl = false, alt = false;
    bool right = false;
    /// Wheel movement in DIPs (positive = scroll content towards larger offsets).
    float wheelX = 0, wheelY = 0;
};

struct KeyEvent {
    int vk = 0;
    bool shift = false, ctrl = false, alt = false;
};

enum class Cursor { Arrow, IBeam, ResizeLeftRight, ResizeUpDown, Hand };

/// What a view tree needs from the window (or offscreen surface) it lives in.
class Host {
public:
    virtual ~Host() = default;
    virtual void invalidate() = 0;
    virtual void setFocus(View* v) = 0;
    virtual View* focus() const = 0;
    virtual void forget(View* v) = 0;
    virtual HWND hwnd() const { return nullptr; }
    /// Shows a native pop-up menu at a point (window coordinates); returns the chosen index or -1.
    virtual int popupMenu(const std::vector<std::string>& items, int checked, Point at) { return -1; }
    /// Converts DIPs in the window to screen pixels (for IME and menus).
    virtual POINT toScreen(Point p) const { return POINT{(LONG)p.x, (LONG)p.y}; }
};

class ScrollView;

class View {
public:
    View() = default;
    virtual ~View();
    View(const View&) = delete;
    View& operator=(const View&) = delete;

    // Geometry.
    const Rect& frame() const { return frame_; }
    void setFrame(const Rect& r);
    Rect bounds() const { return {boundsOrigin_, frame_.size()}; }
    Point boundsOrigin() const { return boundsOrigin_; }
    void setBoundsOrigin(Point p);
    float width() const { return frame_.w; }
    float height() const { return frame_.h; }

    // Hierarchy (non-owning: a view's children are normally members of it).
    View* superview() const { return superview_; }
    const std::vector<View*>& subviews() const { return subviews_; }
    void addSubview(View* v);
    void removeFromSuperview();
    bool hidden() const { return hidden_; }
    void setHidden(bool h);
    /// Hidden itself or inside a hidden ancestor.
    bool isHiddenOrHasHiddenAncestor() const;

    // Host.
    Host* host() const;
    void setHost(Host* h) { host_ = h; }

    // Display and layout.
    void setNeedsDisplay();
    void setNeedsLayout();
    bool needsLayout() const { return needsLayout_; }
    void layoutSubtreeIfNeeded();
    virtual void layout() {}
    virtual void draw(Gfx& g, const Rect& dirty) {}
    /// Drawn after the subviews (borders, scrollers).
    virtual void drawOverlay(Gfx& g, const Rect& dirty) {}
    /// Width/height the view would like; negative = no preference.
    virtual Size intrinsicSize() { return {-1, -1}; }
    void render(Gfx& g, const Rect& visible);

    // Layer-like appearance.
    std::optional<Color> backgroundColor;
    float cornerRadius = 0;
    Color borderColor;
    float borderWidth = 0;
    bool masksToBounds = false;

    // Coordinates.
    Point convertFromWindow(Point w) const;
    Point convertToWindow(Point p) const;
    Rect convertRectToWindow(const Rect& r) const;
    Point localPoint(const MouseEvent& e) const { return convertFromWindow(e.window); }

    // Events. The defaults pass the event to the superview, like AppKit's responder chain.
    virtual View* hitTest(Point inSelf);
    virtual void mouseDown(const MouseEvent& e);
    virtual void mouseDragged(const MouseEvent& e);
    virtual void mouseUp(const MouseEvent& e);
    virtual void mouseMoved(const MouseEvent& e);
    virtual void mouseEntered(const MouseEvent& e) {}
    virtual void mouseExited(const MouseEvent& e) {}
    virtual void scrollWheel(const MouseEvent& e);
    virtual bool acceptsFocus() const { return false; }
    virtual void keyDown(const KeyEvent& e);
    virtual void charInput(wchar_t c) {}
    virtual void focusChanged(bool focused) {}
    /// Edit commands sent through the menu: "cut", "copy", "paste", "selectAll", "undo", "redo", "find".
    virtual bool editCommand(const std::string& cmd) { return false; }
    virtual Cursor cursor(Point inSelf) { return Cursor::Arrow; }
    virtual std::string tooltipAt(Point inSelf) { return tooltip; }
    /// Where the caret is, in window coordinates (for the input method window).
    virtual std::optional<Rect> caretRect() const { return std::nullopt; }

    std::string tooltip;

    // Scrolling.
    ScrollView* enclosingScrollView() const;
    /// Scrolls the enclosing scroll view so that the rect (in this view's coordinates) is visible.
    void scrollToVisible(const Rect& r);

private:
    Rect frame_;
    Point boundsOrigin_;
    View* superview_ = nullptr;
    std::vector<View*> subviews_;
    bool hidden_ = false;
    bool needsLayout_ = true;
    Host* host_ = nullptr;
};

/// The visible part of a scroll view's document; its bounds origin is the scroll offset.
class ClipView : public View {};

/// A view that scrolls a larger document view, with thin overlay scrollers.
class ScrollView : public View {
public:
    ScrollView();
    void setDocumentView(View* v);
    View* documentView() const { return document_; }
    bool hasVerticalScroller = true;
    bool hasHorizontalScroller = false;
    bool drawsBackground = true;
    Color background = Color::white();
    std::function<void()> onScroll;

    Size contentSize() const { return frame().size(); }
    /// The visible part of the document, in document coordinates.
    Rect documentVisibleRect() const { return clip_.bounds(); }
    Point offset() const { return clip_.boundsOrigin(); }
    void scrollTo(Point p);
    void scrollToVisible(const Rect& docRect);
    void clampOffset();

    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void drawOverlay(Gfx& g, const Rect& dirty) override;
    View* hitTest(Point inSelf) override;
    void scrollWheel(const MouseEvent& e) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDragged(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override;
    void mouseMoved(const MouseEvent& e) override {}

private:
    ClipView clip_;
    View* document_ = nullptr;
    int dragging_ = 0;   // 1 vertical thumb, 2 horizontal thumb
    float dragStart_ = 0, dragOffset_ = 0;
    bool needsV() const;
    bool needsH() const;
    Rect vThumb() const;
    Rect hThumb() const;
};

/// Two panes with a draggable divider between them.
class SplitView : public View {
public:
    /// vertical = panes side by side (the divider is a vertical line).
    explicit SplitView(bool vertical = true) : vertical_(vertical) {}
    void setPanes(View* a, View* b);
    void setPosition(float p);
    float position() const { return position_; }
    /// Limits on the divider position, given the current size.
    std::function<float()> minPosition, maxPosition;
    /// Which pane absorbs size changes: 0 = first, 1 = second.
    int resizingPane = 1;
    std::function<void()> onResize;
    Color dividerColor;

    void layout() override;
    void drawOverlay(Gfx& g, const Rect& dirty) override;
    View* hitTest(Point inSelf) override;
    Cursor cursor(Point inSelf) override { return vertical_ ? Cursor::ResizeLeftRight : Cursor::ResizeUpDown; }
    void mouseDown(const MouseEvent& e) override;
    void mouseDragged(const MouseEvent& e) override;
    void mouseUp(const MouseEvent& e) override {}

private:
    bool vertical_;
    View* a_ = nullptr;
    View* b_ = nullptr;
    float position_ = -1;
    float lastLength_ = -1;
    float length() const { return vertical_ ? width() : height(); }
    float clamp(float p) const;
};

}  // namespace ui
