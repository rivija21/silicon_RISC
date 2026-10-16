#include "HostBase.h"

#include "Theme.h"

namespace ui {

HostBase::HostBase(View* root) : root_(root) { root_->setHost(this); }

void HostBase::setFocus(View* v) {
    if (v == focus_) return;
    View* old = focus_;
    focus_ = v;
    if (old) old->focusChanged(false);
    if (v) v->focusChanged(true);
    invalidate();
}

void HostBase::forget(View* v) {
    // Forget the view and its subviews (a hidden or removed subtree can no longer be hovered or focused).
    auto inside = [v](View* x) {
        for (View* a = x; a; a = a->superview())
            if (a == v) return true;
        return false;
    };
    if (focus_ && inside(focus_)) {
        View* f = focus_;
        focus_ = nullptr;
        if (f != v || v->superview()) f->focusChanged(false);
    }
    if (captured_ && inside(captured_)) captured_ = nullptr;
    hoverChain_.erase(std::remove_if(hoverChain_.begin(), hoverChain_.end(), inside), hoverChain_.end());
}

MouseEvent HostBase::makeEvent(Point p, bool shift, bool ctrl, bool alt) const {
    MouseEvent e;
    e.window = p;
    e.shift = shift;
    e.ctrl = ctrl;
    e.alt = alt;
    return e;
}

void HostBase::updateHover(Point p) {
    View* hit = root_->frame().contains(p) ? root_->hitTest(p) : nullptr;
    std::vector<View*> chain;
    for (View* v = hit; v; v = v->superview()) chain.push_back(v);
    MouseEvent e = makeEvent(p);
    for (View* v : hoverChain_)
        if (std::find(chain.begin(), chain.end(), v) == chain.end()) v->mouseExited(e);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        if (std::find(hoverChain_.begin(), hoverChain_.end(), *it) == hoverChain_.end()) (*it)->mouseEntered(e);
    hoverChain_ = chain;
}

void HostBase::mouseDown(Point p, int clickCount, bool shift, bool ctrl, bool alt, bool right) {
    tooltipText.clear();
    View* hit = root_->hitTest(p);
    if (!hit) return;
    MouseEvent e = makeEvent(p, shift, ctrl, alt);
    e.clickCount = clickCount;
    e.right = right;
    captured_ = hit;
    hit->mouseDown(e);
    invalidate();
}

void HostBase::mouseMove(Point p, bool buttonDown) {
    if (captured_ && buttonDown) {
        captured_->mouseDragged(makeEvent(p));
        return;
    }
    updateHover(p);
    if (!hoverChain_.empty()) hoverChain_.front()->mouseMoved(makeEvent(p));
}

void HostBase::mouseUp(Point p) {
    View* c = captured_;
    captured_ = nullptr;
    if (c) c->mouseUp(makeEvent(p));
    updateHover(p);
    invalidate();
}

void HostBase::mouseLeave() {
    MouseEvent e = makeEvent({-1, -1});
    auto chain = hoverChain_;
    hoverChain_.clear();
    for (View* v : chain) v->mouseExited(e);
    tooltipText.clear();
    invalidate();
}

void HostBase::wheel(Point p, float dx, float dy, bool shift) {
    tooltipText.clear();
    View* hit = root_->hitTest(p);
    if (!hit) return;
    MouseEvent e = makeEvent(p, shift);
    e.wheelX = dx;
    e.wheelY = dy;
    hit->scrollWheel(e);
    updateHover(p);
    invalidate();
}

void HostBase::key(int vk, bool shift, bool ctrl, bool alt) {
    tooltipText.clear();
    if (focus_ && !focus_->isHiddenOrHasHiddenAncestor()) focus_->keyDown(KeyEvent{vk, shift, ctrl, alt});
}

void HostBase::character(wchar_t c) {
    if (focus_ && !focus_->isHiddenOrHasHiddenAncestor()) focus_->charInput(c);
}

bool HostBase::editCommand(const std::string& cmd) {
    for (View* v = focus_; v; v = v->superview())
        if (v->editCommand(cmd)) return true;
    return false;
}

Cursor HostBase::cursorAt(Point p) {
    if (captured_) return captured_->cursor(captured_->convertFromWindow(p));
    View* hit = root_->hitTest(p);
    return hit ? hit->cursor(hit->convertFromWindow(p)) : Cursor::Arrow;
}

std::string HostBase::tooltipAt(Point p) {
    for (View* v = root_->hitTest(p); v; v = v->superview()) {
        std::string t = v->tooltipAt(v->convertFromWindow(p));
        if (!t.empty()) return t;
    }
    return "";
}

void HostBase::renderTree(Gfx& g, Size size) {
    root_->setFrame({0, 0, size.w, size.h});
    // Layout may change frames that need another pass (e.g. a list growing); two passes settle it.
    root_->layoutSubtreeIfNeeded();
    root_->layoutSubtreeIfNeeded();
    setCurrentGfx(&g);
    g.rt()->Clear(d2d(Theme::background));
    root_->render(g, root_->bounds());
    if (!tooltipText.empty()) {
        TextBox box;
        box.wrap = true;
        box.maxWidth = 360;
        IDWriteTextLayout* l = TextEngine::get().layout(widen(tooltipText), Fonts::small, box);
        DWRITE_TEXT_METRICS m{};
        if (l) l->GetMetrics(&m);
        Rect r{tooltipPoint.x + 12, tooltipPoint.y + 18, m.width + 16, m.height + 10};
        if (r.maxX() > size.w - 4) r.x = std::max(4.0f, size.w - 4 - r.w);
        if (r.maxY() > size.h - 4) r.y = tooltipPoint.y - r.h - 6;
        g.fillRounded(r.offsetBy(0, 1.5f), 6, Color::hex(0x000000, 0.12f));
        g.fillRounded(r, 6, Color::hex(0x2B2A33));
        g.drawLayout(l, {r.x + 8, r.y + 5}, Color::white());
    }
    setCurrentGfx(nullptr);
}

}  // namespace ui
