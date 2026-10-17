#include "Widgets.h"

namespace ui {

// MARK: - TButton

TButton::TButton(std::string title, std::optional<Icon> icon, Style style) : title_(std::move(title)), icon_(icon), style_(style) {}

void TButton::setTitle(const std::string& t) {
    if (t == title_) return;
    title_ = t;
    setNeedsDisplay();
    if (superview()) superview()->setNeedsLayout();
}

void TButton::setIcon(std::optional<Icon> i) {
    icon_ = i;
    setNeedsDisplay();
}

void TButton::setEnabled(bool e) {
    if (e == enabled_) return;
    enabled_ = e;
    setNeedsDisplay();
}

void TButton::setOn(bool on) {
    on_ = on;
    setNeedsDisplay();
}

Size TButton::intrinsicSize() {
    float w = title_.empty() ? 0 : textSize(title_, font).w;
    float iconW = !icon_ ? 0 : (title_.empty() ? 16 : 20);
    return {w + iconW + (title_.empty() ? 14 : 20), 26};
}

void TButton::mouseEntered(const MouseEvent&) {
    hover_ = true;
    setNeedsDisplay();
}

void TButton::mouseExited(const MouseEvent&) {
    hover_ = false;
    setNeedsDisplay();
}

void TButton::mouseDown(const MouseEvent& e) {
    if (!enabled_) return;
    pressed_ = true;
    setNeedsDisplay();
}

void TButton::mouseUp(const MouseEvent& e) {
    if (!enabled_) return;
    pressed_ = false;
    setNeedsDisplay();
    if (bounds().contains(localPoint(e)) && action) action();
}

void TButton::draw(Gfx& g, const Rect& dirty) {
    Rect r = bounds().insetBy(0.5f, 0.5f);
    Color fill = Theme::panel;
    Color fg = enabled_ ? Theme::text : Theme::faint;
    Color border = Theme::borderStrong;
    switch (style_) {
    case Style::Primary:
        fill = enabled_ ? Theme::ink : Theme::faint;
        fg = Color::white();
        border = fill;
        if (hover_ && enabled_) fill = Theme::ink.blended(0.15f, Color::black());
        break;
    case Style::Toggle:
        if (on_) {
            fill = Color::hex(0xE2EAF6);
            fg = Theme::ink;
            border = Theme::ink.alpha(0.55f);
        } else if (hover_ && enabled_) {
            fill = Theme::hover;
        }
        break;
    case Style::Plain:
        if (hover_ && enabled_) fill = Theme::hover;
        break;
    case Style::Link:
        fill = Color::clear();
        border = Color::clear();
        fg = enabled_ ? Theme::ink : Theme::faint;
        break;
    }
    if (pressed_) fill = fill.blended(0.1f, Color::black());
    if (style_ != Style::Link) {
        fillRounded(r, 6, fill);
        strokeRounded(r, 6, border);
    }
    Size want = intrinsicSize();
    float x = (width() - want.w) / 2 + (title_.empty() ? 7 : 10);
    if (icon_) {
        g.drawIcon(*icon_, {x, (height() - 16) / 2, 16, 16}, fg);
        x += title_.empty() ? 16 : 20;
    }
    if (!title_.empty()) {
        Size sz = textSize(title_, font);
        drawText(title_, Point(x, (height() - sz.h) / 2), font, fg);
    }
}

// MARK: - Segmented

Segmented::Segmented(std::vector<std::string> options, int selected) : options_(std::move(options)), selected_(selected) {}

void Segmented::setOptions(const std::vector<std::string>& o) {
    if (o == options_) return;
    options_ = o;
    setNeedsDisplay();
    if (superview()) superview()->setNeedsLayout();
}

void Segmented::setSelected(int k) {
    if (k == selected_) return;
    selected_ = k;
    setNeedsDisplay();
}

void Segmented::setEnabledMask(std::optional<std::vector<bool>> m) {
    enabledMask_ = std::move(m);
    setNeedsDisplay();
}

std::vector<float> Segmented::widths() const {
    std::vector<float> w;
    for (auto& o : options_) w.push_back(textSize(o, font).w + 18);
    return w;
}

Size Segmented::intrinsicSize() {
    float total = 0;
    for (float w : widths()) total += w;
    return {total + 4, 26};
}

void Segmented::draw(Gfx& g, const Rect& dirty) {
    Rect r = bounds().insetBy(0.5f, 0.5f);
    fillRounded(r, 7, Color::hex(0xEFE8DA));
    strokeRounded(r, 7, Theme::border);
    auto ws = widths();
    float total = 0;
    for (float w : ws) total += w;
    float scale = (width() - 4) / std::max(1.0f, total);
    float x = 2;
    for (int k = 0; k < (int)options_.size(); k++) {
        float w = ws[k] * scale;
        Rect seg{x, 2, w, height() - 4};
        bool enabled = !enabledMask_ || (k < (int)enabledMask_->size() && (*enabledMask_)[k]);
        if (k == selected_) drawCard(seg, 5);
        drawText(options_[k], seg, font, !enabled ? Theme::faint : k == selected_ ? Theme::ink : Theme::secondary, Align::Center);
        x += w;
    }
}

void Segmented::mouseDown(const MouseEvent& e) {
    Point p = localPoint(e);
    auto ws = widths();
    float total = 0;
    for (float w : ws) total += w;
    float scale = (width() - 4) / std::max(1.0f, total);
    float x = 2;
    for (int k = 0; k < (int)ws.size(); k++) {
        float w = ws[k] * scale;
        if (p.x >= x && p.x < x + w) {
            bool enabled = !enabledMask_ || (k < (int)enabledMask_->size() && (*enabledMask_)[k]);
            if (enabled && k != selected_) {
                setSelected(k);
                if (onChange) onChange(k);
            }
            return;
        }
        x += w;
    }
}

// MARK: - Check

Check::Check(std::string title, bool on) : title_(std::move(title)), on_(on) {}

void Check::setOn(bool on) {
    if (on == on_) return;
    on_ = on;
    setNeedsDisplay();
}

Size Check::intrinsicSize() { return {textSize(title_, font).w + 24, 20}; }

void Check::draw(Gfx& g, const Rect& dirty) {
    Rect box{1, (height() - 14) / 2, 14, 14};
    fillRounded(box, 3.5f, on_ ? Theme::ink : Theme::panel);
    strokeRounded(box, 3.5f, on_ ? Theme::ink : Theme::borderStrong);
    if (on_) {
        g.strokePolyline({Point(box.minX() + 3.2f, box.midY()), Point(box.minX() + 6, box.maxY() - 3.5f), Point(box.maxX() - 3, box.minY() + 3.5f)},
                         Color::white(), 1.8f);
    }
    drawText(title_, Rect(21, 0, width() - 21, height()), font, Theme::text);
}

void Check::mouseDown(const MouseEvent& e) {
    setOn(!on_);
    if (onChange) onChange(on_);
}

// MARK: - PopUp

PopUp::PopUp(std::vector<std::string> items, int selected) : items_(std::move(items)), selected_(selected) {}

void PopUp::set(const std::vector<std::string>& items, int selected) {
    if (items == items_ && selected == selected_) return;
    items_ = items;
    selected_ = selected;
    setNeedsDisplay();
}

void PopUp::select(int k) {
    if (k == selected_) return;
    selected_ = k;
    setNeedsDisplay();
}

void PopUp::setEnabled(bool e) {
    if (e == enabled_) return;
    enabled_ = e;
    setNeedsDisplay();
}

Size PopUp::intrinsicSize() {
    float w = 0;
    for (auto& i : items_) w = std::max(w, textSize(i, font).w);
    return {w + 40, 26};
}

void PopUp::mouseEntered(const MouseEvent&) {
    hover_ = true;
    setNeedsDisplay();
}

void PopUp::mouseExited(const MouseEvent&) {
    hover_ = false;
    setNeedsDisplay();
}

void PopUp::draw(Gfx& g, const Rect& dirty) {
    Rect r = bounds().insetBy(0.5f, 1.5f);
    fillRounded(r, 6, hover_ && enabled_ ? Theme::hover : Theme::panel);
    strokeRounded(r, 6, Theme::borderStrong);
    std::string shown = selected_ >= 0 && selected_ < (int)items_.size() ? items_[selected_] : "";
    // Narrow pop-ups shrink their text a little before truncating it.
    Font f = font;
    float avail = width() - 30, w = textSize(shown, f).w;
    if (w > avail) f.size = std::max(10.0f, f.size * avail / w);
    drawText(shown, Rect(9, 0, avail + 1, height()), f, enabled_ ? Theme::text : Theme::faint);
    // Up/down chevrons on the right, like a macOS pop-up button.
    float cx = width() - 14, cy = height() / 2;
    Color c = enabled_ ? Theme::secondary : Theme::faint;
    g.strokePolyline({Point(cx - 3.5f, cy - 2), Point(cx, cy - 5.5f), Point(cx + 3.5f, cy - 2)}, c, 1.4f);
    g.strokePolyline({Point(cx - 3.5f, cy + 2), Point(cx, cy + 5.5f), Point(cx + 3.5f, cy + 2)}, c, 1.4f);
}

void PopUp::mouseDown(const MouseEvent& e) {
    if (!enabled_ || items_.empty()) return;
    Host* h = host();
    if (!h) return;
    int k = h->popupMenu(items_, selected_, convertToWindow({0, height()}));
    hover_ = false;
    setNeedsDisplay();
    if (k >= 0 && k != selected_) {
        selected_ = k;
        if (onChange) onChange(k);
    }
}

// MARK: - Label

Label::Label(std::string text, Font f, Color color) : font(f), text_(std::move(text)), color_(color) {}

void Label::setText(const std::string& s) {
    if (s == text_) return;
    text_ = s;
    setNeedsDisplay();
}

void Label::setColor(const Color& c) {
    color_ = c;
    setNeedsDisplay();
}

void Label::draw(Gfx& g, const Rect& dirty) {
    if (wraps) {
        TextBox box;
        box.wrap = true;
        box.truncate = true;
        box.align = align;
        g.drawTextBox(text_, bounds(), font, color_, box);
    } else {
        drawText(text_, bounds(), font, color_, align);
    }
}

// MARK: - Slider

void Slider::setValue(int v) {
    v = std::max(0, std::min(v, ticks_ - 1));
    if (v == value_) return;
    value_ = v;
    setNeedsDisplay();
}

void Slider::draw(Gfx& g, const Rect& dirty) {
    float x0 = 7, x1 = width() - 7, cy = height() / 2;
    fillRounded({x0, cy - 2, x1 - x0, 4}, 2, Color::hex(0xE3D9C6));
    float step = (x1 - x0) / std::max(1, ticks_ - 1);
    fillRounded({x0, cy - 2, step * value_, 4}, 2, Theme::ink.alpha(0.6f));
    for (int k = 0; k < ticks_; k++) line(Point(x0 + k * step, cy + 5), Point(x0 + k * step, cy + 8), Theme::borderStrong);
    Rect knob{x0 + step * value_ - 6, cy - 6, 12, 12};
    g.fillEllipse(knob.offsetBy(0, 0.8f), Color::hex(0x6B5A3A, 0.15f));
    g.fillEllipse(knob, Theme::panel);
    g.strokeRounded(knob, 6, Theme::borderStrong);
}

void Slider::track(const MouseEvent& e) {
    Point p = localPoint(e);
    float x0 = 7, x1 = width() - 7;
    float step = (x1 - x0) / std::max(1, ticks_ - 1);
    int v = (int)std::lround((p.x - x0) / std::max(1.0f, step));
    v = std::max(0, std::min(v, ticks_ - 1));
    if (v != value_) {
        setValue(v);
        if (onChange) onChange(v);
    }
}

void Slider::mouseDown(const MouseEvent& e) { track(e); }
void Slider::mouseDragged(const MouseEvent& e) { track(e); }

}  // namespace ui
