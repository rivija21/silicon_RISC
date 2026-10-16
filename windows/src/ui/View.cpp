#include "View.h"

namespace ui {

View::~View() {
    if (Host* h = host()) h->forget(this);
    for (View* c : subviews_) c->superview_ = nullptr;
    if (superview_) {
        auto& s = superview_->subviews_;
        s.erase(std::remove(s.begin(), s.end(), this), s.end());
    }
}

void View::setFrame(const Rect& r) {
    if (r == frame_) return;
    bool resized = r.w != frame_.w || r.h != frame_.h;
    frame_ = r;
    if (resized) needsLayout_ = true;
    setNeedsDisplay();
}

void View::setBoundsOrigin(Point p) {
    if (p == boundsOrigin_) return;
    boundsOrigin_ = p;
    setNeedsDisplay();
}

void View::addSubview(View* v) {
    if (v->superview_ == this) return;
    v->removeFromSuperview();
    v->superview_ = this;
    subviews_.push_back(v);
    v->needsLayout_ = true;
    setNeedsLayout();
}

void View::removeFromSuperview() {
    if (!superview_) return;
    if (Host* h = host()) h->forget(this);
    auto& s = superview_->subviews_;
    s.erase(std::remove(s.begin(), s.end(), this), s.end());
    superview_->setNeedsDisplay();
    superview_ = nullptr;
}

void View::setHidden(bool h) {
    if (h == hidden_) return;
    hidden_ = h;
    if (h)
        if (Host* x = host()) x->forget(this);
    if (!h) needsLayout_ = true;
    if (superview_) superview_->setNeedsDisplay();
    setNeedsLayout();
}

bool View::isHiddenOrHasHiddenAncestor() const {
    for (const View* v = this; v; v = v->superview_)
        if (v->hidden_) return true;
    return false;
}

Host* View::host() const {
    const View* v = this;
    while (v->superview_) v = v->superview_;
    return v->host_;
}

void View::setNeedsDisplay() {
    if (Host* h = host()) h->invalidate();
}

void View::setNeedsLayout() {
    // The layout pass visits every visible view, so flagging this one is enough.
    needsLayout_ = true;
    setNeedsDisplay();
}

void View::layoutSubtreeIfNeeded() {
    if (needsLayout_) {
        needsLayout_ = false;
        layout();
    }
    for (size_t k = 0; k < subviews_.size(); k++) {
        View* c = subviews_[k];
        if (!c->hidden_) c->layoutSubtreeIfNeeded();
    }
}

void View::render(Gfx& g, const Rect& visible) {
    Rect b = bounds();
    bool rounded = cornerRadius > 0 && masksToBounds;
    if (rounded) g.pushRoundedClip(b, cornerRadius);
    if (backgroundColor) {
        if (cornerRadius > 0) g.fillRounded(b, cornerRadius, *backgroundColor);
        else g.fillRect(visible, *backgroundColor);
    }
    draw(g, visible);
    for (size_t k = 0; k < subviews_.size(); k++) {
        View* c = subviews_[k];
        if (c->hidden_) continue;
        Rect cf = c->frame_;
        Rect vis = cf.intersection(visible);
        if (vis.isEmpty()) continue;
        auto saved = g.transform();
        float dx = cf.x - c->boundsOrigin_.x, dy = cf.y - c->boundsOrigin_.y;
        g.translate(dx, dy);
        Rect childVisible = vis.offsetBy(-dx, -dy);
        g.pushClip(childVisible);
        c->render(g, childVisible);
        g.popClip();
        g.setTransform(saved);
    }
    drawOverlay(g, visible);
    if (rounded) g.popRoundedClip();
    if (borderWidth > 0 && !borderColor.isClear()) {
        if (cornerRadius > 0) g.strokeRounded(b, cornerRadius, borderColor, borderWidth);
        else g.strokeRect(b.insetBy(borderWidth / 2, borderWidth / 2), borderColor, borderWidth);
    }
}

Point View::convertFromWindow(Point w) const {
    Point p = superview_ ? superview_->convertFromWindow(w) : w;
    return {p.x - frame_.x + boundsOrigin_.x, p.y - frame_.y + boundsOrigin_.y};
}

Point View::convertToWindow(Point p) const {
    Point q{p.x + frame_.x - boundsOrigin_.x, p.y + frame_.y - boundsOrigin_.y};
    return superview_ ? superview_->convertToWindow(q) : q;
}

Rect View::convertRectToWindow(const Rect& r) const {
    Point o = convertToWindow(r.origin());
    return {o, r.size()};
}

View* View::hitTest(Point p) {
    for (auto it = subviews_.rbegin(); it != subviews_.rend(); ++it) {
        View* c = *it;
        if (c->hidden_ || !c->frame_.contains(p)) continue;
        Point cp{p.x - c->frame_.x + c->boundsOrigin_.x, p.y - c->frame_.y + c->boundsOrigin_.y};
        if (View* v = c->hitTest(cp)) return v;
    }
    return this;
}

void View::mouseDown(const MouseEvent& e) {
    if (superview_) superview_->mouseDown(e);
}
void View::mouseDragged(const MouseEvent& e) {
    if (superview_) superview_->mouseDragged(e);
}
void View::mouseUp(const MouseEvent& e) {
    if (superview_) superview_->mouseUp(e);
}
void View::mouseMoved(const MouseEvent& e) {
    if (superview_) superview_->mouseMoved(e);
}
void View::scrollWheel(const MouseEvent& e) {
    if (superview_) superview_->scrollWheel(e);
}
void View::keyDown(const KeyEvent& e) {
    if (superview_) superview_->keyDown(e);
}

ScrollView* View::enclosingScrollView() const {
    for (View* v = superview_; v; v = v->superview_)
        if (auto* s = dynamic_cast<ScrollView*>(v)) return s;
    return nullptr;
}

void View::scrollToVisible(const Rect& r) {
    ScrollView* s = enclosingScrollView();
    if (!s || !s->documentView()) return;
    Rect w = convertRectToWindow(r);
    Point d = s->documentView()->convertFromWindow(w.origin());
    s->scrollToVisible({d, r.size()});
}

// MARK: - Scroll view

ScrollView::ScrollView() { addSubview(&clip_); }

void ScrollView::setDocumentView(View* v) {
    if (document_) document_->removeFromSuperview();
    document_ = v;
    clip_.addSubview(v);
    setNeedsLayout();
}

void ScrollView::layout() {
    clip_.setFrame(bounds());
    clampOffset();
}

void ScrollView::clampOffset() {
    if (!document_) return;
    Size doc = document_->frame().size();
    Size vis = contentSize();
    Point o = clip_.boundsOrigin();
    o.x = std::max(0.0f, std::min(o.x, doc.w - vis.w));
    o.y = std::max(0.0f, std::min(o.y, doc.h - vis.h));
    if (!(o == clip_.boundsOrigin())) {
        clip_.setBoundsOrigin(o);
        if (onScroll) onScroll();
    }
}

void ScrollView::scrollTo(Point p) {
    Point before = clip_.boundsOrigin();
    clip_.setBoundsOrigin(p);
    clampOffset();
    if (!(before == clip_.boundsOrigin()) && onScroll) onScroll();
}

void ScrollView::scrollToVisible(const Rect& r) {
    Rect vis = documentVisibleRect();
    Point o = vis.origin();
    if (r.maxX() > vis.maxX()) o.x = r.maxX() - vis.w;
    if (r.minX() < o.x) o.x = r.minX();
    if (r.maxY() > vis.maxY()) o.y = r.maxY() - vis.h;
    if (r.minY() < o.y) o.y = r.minY();
    scrollTo(o);
}

bool ScrollView::needsV() const { return hasVerticalScroller && document_ && document_->height() > height() + 0.5f; }
bool ScrollView::needsH() const { return hasHorizontalScroller && document_ && document_->width() > width() + 0.5f; }

Rect ScrollView::vThumb() const {
    float docH = document_->height(), visH = height();
    float track = height() - (needsH() ? 10 : 0) - 4;
    float h = std::max(24.0f, track * visH / docH);
    float y = 2 + (track - h) * (offset().y / std::max(1.0f, docH - visH));
    return {width() - 9, y, 7, h};
}

Rect ScrollView::hThumb() const {
    float docW = document_->width(), visW = width();
    float track = width() - (needsV() ? 10 : 0) - 4;
    float w = std::max(24.0f, track * visW / docW);
    float x = 2 + (track - w) * (offset().x / std::max(1.0f, docW - visW));
    return {x, height() - 9, w, 7};
}

void ScrollView::draw(Gfx& g, const Rect& dirty) {
    if (drawsBackground) g.fillRect(dirty, background);
}

void ScrollView::drawOverlay(Gfx& g, const Rect& dirty) {
    Color thumb = Color::hex(0x7C7364, dragging_ ? 0.55f : 0.32f);
    if (needsV()) g.fillRounded(vThumb(), 3.5f, thumb);
    if (needsH()) g.fillRounded(hThumb(), 3.5f, thumb);
}

View* ScrollView::hitTest(Point p) {
    if (needsV() && p.x >= width() - 11) return this;
    if (needsH() && p.y >= height() - 11) return this;
    return View::hitTest(p);
}

void ScrollView::scrollWheel(const MouseEvent& e) {
    if (!document_) return;
    Point o = offset();
    float dx = e.wheelX, dy = e.wheelY;
    if (e.shift && dx == 0) std::swap(dx, dy);
    if (!needsV()) {
        if (needsH() && dx == 0) std::swap(dx, dy);
        else dy = 0;
    }
    if (!needsH()) dx = 0;
    if (dx == 0 && dy == 0) {
        View::scrollWheel(e);
        return;
    }
    scrollTo({o.x + dx, o.y + dy});
}

void ScrollView::mouseDown(const MouseEvent& e) {
    Point p = localPoint(e);
    if (needsV() && p.x >= width() - 11) {
        Rect t = vThumb();
        if (!t.contains({t.x + 1, p.y})) {
            // Page towards the click.
            float page = height() * 0.9f;
            scrollTo({offset().x, offset().y + (p.y < t.y ? -page : page)});
            return;
        }
        dragging_ = 1;
        dragStart_ = p.y;
        dragOffset_ = offset().y;
        setNeedsDisplay();
        return;
    }
    if (needsH() && p.y >= height() - 11) {
        Rect t = hThumb();
        if (!t.contains({p.x, t.y + 1})) {
            float page = width() * 0.9f;
            scrollTo({offset().x + (p.x < t.x ? -page : page), offset().y});
            return;
        }
        dragging_ = 2;
        dragStart_ = p.x;
        dragOffset_ = offset().x;
        setNeedsDisplay();
        return;
    }
    View::mouseDown(e);
}

void ScrollView::mouseDragged(const MouseEvent& e) {
    Point p = localPoint(e);
    if (dragging_ == 1) {
        float docH = document_->height(), visH = height();
        float track = height() - (needsH() ? 10 : 0) - 4;
        float h = vThumb().h;
        float scale = (docH - visH) / std::max(1.0f, track - h);
        scrollTo({offset().x, dragOffset_ + (p.y - dragStart_) * scale});
    } else if (dragging_ == 2) {
        float docW = document_->width(), visW = width();
        float track = width() - (needsV() ? 10 : 0) - 4;
        float w = hThumb().w;
        float scale = (docW - visW) / std::max(1.0f, track - w);
        scrollTo({dragOffset_ + (p.x - dragStart_) * scale, offset().y});
    } else {
        View::mouseDragged(e);
    }
}

void ScrollView::mouseUp(const MouseEvent& e) {
    if (dragging_) {
        dragging_ = 0;
        setNeedsDisplay();
        return;
    }
    View::mouseUp(e);
}

// MARK: - Split view

void SplitView::setPanes(View* a, View* b) {
    a_ = a;
    b_ = b;
    addSubview(a);
    addSubview(b);
}

float SplitView::clamp(float p) const {
    float lo = minPosition ? minPosition() : 0;
    float hi = maxPosition ? maxPosition() : length();
    if (hi < lo) hi = lo;
    return std::max(lo, std::min(p, hi));
}

void SplitView::setPosition(float p) {
    position_ = clamp(p);
    lastLength_ = length();
    setNeedsLayout();
    if (onResize) onResize();
}

void SplitView::layout() {
    float len = length();
    if (position_ < 0) position_ = len / 2;
    if (lastLength_ >= 0 && len != lastLength_ && resizingPane == 0) position_ += len - lastLength_;
    lastLength_ = len;
    position_ = std::max(0.0f, std::min(position_, len - 1));
    if (!a_ || !b_) return;
    if (vertical_) {
        a_->setFrame({0, 0, position_, height()});
        b_->setFrame({position_ + 1, 0, std::max(0.0f, width() - position_ - 1), height()});
    } else {
        a_->setFrame({0, 0, width(), position_});
        b_->setFrame({0, position_ + 1, width(), std::max(0.0f, height() - position_ - 1)});
    }
    if (onResize) onResize();
}

void SplitView::drawOverlay(Gfx& g, const Rect& dirty) {
    if (dividerColor.isClear()) return;
    if (vertical_) g.fillRect({position_, 0, 1, height()}, dividerColor);
    else g.fillRect({0, position_, width(), 1}, dividerColor);
}

View* SplitView::hitTest(Point p) {
    float c = vertical_ ? p.x : p.y;
    if (c >= position_ - 3 && c <= position_ + 4) return this;
    return View::hitTest(p);
}

void SplitView::mouseDown(const MouseEvent& e) {}

void SplitView::mouseDragged(const MouseEvent& e) {
    Point p = localPoint(e);
    setPosition(vertical_ ? p.x : p.y);
}

}  // namespace ui
