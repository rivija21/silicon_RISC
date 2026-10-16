#include "ListView.h"

namespace ui {

Row Row::header(const std::string& s, const std::string& detail) {
    AttrString a;
    a.add(s, Fonts::subheading, Theme::heading);
    if (!detail.empty()) a.add("   " + detail, Fonts::small, Theme::secondary);
    Row r({Cell(a)}, Kind::Header, 28);
    return r;
}

void ListView::setRows(std::vector<Row> rows) {
    rows_ = std::move(rows);
    offsets_.clear();
    float y = 6;
    for (auto& r : rows_) {
        offsets_.push_back(y);
        y += r.height;
    }
    contentHeight_ = y + 8;
    if (hovered_ && *hovered_ >= (int)rows_.size()) hovered_.reset();
    Rect f = frame();
    f.h = std::max(contentHeight_, superview() ? superview()->height() : 0.0f);
    setFrame(f);
    setNeedsDisplay();
}

std::optional<int> ListView::indexAt(float y) const {
    if (offsets_.empty()) return std::nullopt;
    int lo = 0, hi = (int)offsets_.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (offsets_[mid] <= y) lo = mid;
        else hi = mid - 1;
    }
    if (y < offsets_[lo] || y >= offsets_[lo] + rows_[lo].height) return std::nullopt;
    return lo;
}

Rect ListView::rectOfRow(int k) const { return {0, offsets_[k], width(), rows_[k].height}; }

void ListView::reveal(int k) {
    ScrollView* sv = enclosingScrollView();
    if (k < 0 || k >= (int)rows_.size() || !sv) return;
    Rect r = rectOfRow(k);
    Rect vis = sv->documentVisibleRect();
    if (r.minY() < vis.minY() + 4 || r.maxY() > vis.maxY() - 4) {
        float y = std::max(0.0f, std::min(r.midY() - vis.h / 2, height() - vis.h));
        sv->scrollTo({vis.minX(), y});
    }
}

void ListView::setHovered(std::optional<int> h) {
    if (h == hovered_) return;
    hovered_ = h;
    setNeedsDisplay();
}

void ListView::mouseMoved(const MouseEvent& e) {
    auto k = indexAt(localPoint(e).y);
    setHovered(k && rows_[*k].kind == Row::Kind::Normal ? k : std::nullopt);
}

void ListView::mouseExited(const MouseEvent&) { setHovered(std::nullopt); }

void ListView::mouseDown(const MouseEvent& e) {
    auto k = indexAt(localPoint(e).y);
    if (k && rows_[*k].kind == Row::Kind::Normal && onClick) {
        int idx = *k;
        Row r = rows_[idx];
        onClick(idx, r);
    }
}

std::string ListView::tooltipAt(Point p) {
    auto k = indexAt(p.y);
    if (k) return rows_[*k].tooltip;
    return "";
}

void ListView::draw(Gfx& g, const Rect& dirty) {
    g.fillRect(dirty, Theme::panel);
    if (rows_.empty()) return;
    int first = indexAt(std::max(6.0f, dirty.minY())).value_or(0);
    for (int k = std::max(0, first - 1); k < (int)rows_.size(); k++) {
        float y = offsets_[k];
        if (y > dirty.maxY()) break;
        drawRow(g, rows_[k], k, y);
    }
}

void ListView::drawRow(Gfx& g, const Row& r, int k, float y) {
    Rect full{0, y, width(), r.height};
    switch (r.kind) {
    case Row::Kind::Spacer: return;
    case Row::Kind::Header:
        if (!r.cells.empty()) {
            float h = r.cells[0].text.size().h;
            g.drawAttributed(r.cells[0].text, {padding + r.indent, y + r.height - h - 4});
        }
        line({padding, y + r.height - 1.5f}, {width() - padding, y + r.height - 1.5f}, Theme::border);
        return;
    case Row::Kind::Section: fillRect(full, Theme::panelAlt); break;
    case Row::Kind::Normal: break;
    }
    if (r.background) fillRect(full, *r.background);
    if (highlight)
        if (auto h = highlight(r)) fillRect(full, *h);
    if (hovered_ == k) fillRect(full, Theme::hover.alpha(0.7f));
    if (r.marker) fillRect({0, y + 1, 4, r.height - 2}, *r.marker);
    float x = padding + r.indent;
    float fixed = 0;
    int flexCount = 0;
    for (auto& c : r.cells) {
        if (c.width) fixed += *c.width;
        else flexCount++;
    }
    float flexW = std::max(40.0f, (width() - padding * 2 - r.indent - fixed) / std::max(1, flexCount));
    for (auto& c : r.cells) {
        float w = c.width ? *c.width : flexW;
        Rect cellR{x + c.leftPad, y, w - c.leftPad - 4, r.height};
        Size ts = c.text.size();
        if (c.badge) {
            float tw = std::min(cellR.w, ts.w + 10);
            float bx = c.align == Align::Right ? cellR.maxX() - tw : c.align == Align::Center ? cellR.midX() - tw / 2 : cellR.minX();
            fillRounded({bx, y + 2, tw, r.height - 4}, 4, *c.badge);
            TextBox box;
            box.truncate = true;
            g.drawAttributed(c.text, {bx + 5, y + (r.height - ts.h) / 2, tw - 6, ts.h + 2}, box);
        } else {
            TextBox box;
            box.truncate = true;
            box.align = c.align;
            g.drawAttributed(c.text, {cellR.minX(), y + (r.height - ts.h) / 2, cellR.w, ts.h + 2}, box);
        }
        x += w;
    }
}

// MARK: - Panels

ListPanel::ListPanel() {
    scroll.setDocumentView(&list);
    scroll.hasVerticalScroller = true;
    scroll.background = Theme::panel;
    addSubview(&scroll);
}

void ListPanel::layout() {
    scroll.setFrame(bounds());
    Size cs = scroll.contentSize();
    list.setFrame({0, 0, std::max(cs.w, list.minWidth), std::max(list.contentHeight(), cs.h)});
    scroll.clampOffset();
}

void ListPanel::set(std::vector<Row> rows) {
    list.setRows(std::move(rows));
    setNeedsLayout();
    layoutSubtreeIfNeeded();
}

CanvasPanel::CanvasPanel(View* c) : canvas(c) {
    scroll.setDocumentView(canvas);
    scroll.hasVerticalScroller = true;
    scroll.hasHorizontalScroller = true;
    scroll.background = Theme::panel;
    addSubview(&scroll);
}

void CanvasPanel::layout() {
    scroll.setFrame(bounds());
    Size want = canvas->intrinsicSize();
    Size cs = scroll.contentSize();
    canvas->setFrame({0, 0, std::max(want.w, cs.w), std::max(want.h, cs.h)});
    scroll.clampOffset();
}

}  // namespace ui
