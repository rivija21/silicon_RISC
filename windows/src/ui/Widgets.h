// Small controls drawn in the app's style: buttons, segmented controls,
// check boxes, pop-up menus, labels and a slider.
#pragma once

#include <functional>
#include <optional>

#include "Theme.h"
#include "View.h"

namespace ui {

/// A soft, rounded button. Can show an icon and act as a toggle.
class TButton : public View {
public:
    enum class Style { Plain, Primary, Toggle, Link };
    TButton(std::string title, std::optional<Icon> icon = std::nullopt, Style style = Style::Plain);
    void setTitle(const std::string& t);
    const std::string& title() const { return title_; }
    void setIcon(std::optional<Icon> i);
    void setEnabled(bool e);
    bool enabled() const { return enabled_; }
    void setOn(bool on);
    std::function<void()> action;
    Font font = Fonts::ui(12, Weight::Medium);

    Size intrinsicSize() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseEntered(const MouseEvent&) override;
    void mouseExited(const MouseEvent&) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDragged(const MouseEvent& e) override {}
    void mouseUp(const MouseEvent& e) override;

private:
    std::string title_;
    std::optional<Icon> icon_;
    Style style_;
    bool on_ = false, enabled_ = true, hover_ = false, pressed_ = false;
};

/// A row of mutually exclusive options.
class Segmented : public View {
public:
    explicit Segmented(std::vector<std::string> options, int selected = 0);
    void setOptions(const std::vector<std::string>& o);
    const std::vector<std::string>& options() const { return options_; }
    void setSelected(int k);
    int selected() const { return selected_; }
    void setEnabledMask(std::optional<std::vector<bool>> m);
    std::function<void(int)> onChange;
    Font font = Fonts::ui(11.5f, Weight::Medium);

    Size intrinsicSize() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDragged(const MouseEvent&) override {}
    void mouseUp(const MouseEvent&) override {}

private:
    std::vector<std::string> options_;
    int selected_;
    std::optional<std::vector<bool>> enabledMask_;
    std::vector<float> widths() const;
};

/// A checkbox with a label.
class Check : public View {
public:
    Check(std::string title, bool on);
    void setOn(bool on);
    bool on() const { return on_; }
    std::function<void(bool)> onChange;
    Font font = Fonts::ui(11.5f);

    Size intrinsicSize() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDragged(const MouseEvent&) override {}
    void mouseUp(const MouseEvent&) override {}

private:
    std::string title_;
    bool on_;
};

/// A pop-up menu button: shows the selected item, opens a native menu when clicked.
class PopUp : public View {
public:
    explicit PopUp(std::vector<std::string> items, int selected = 0);
    void set(const std::vector<std::string>& items, int selected);
    void select(int k);
    int selected() const { return selected_; }
    void setEnabled(bool e);
    std::function<void(int)> onChange;
    Font font = Fonts::ui(12);

    Size intrinsicSize() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseEntered(const MouseEvent&) override;
    void mouseExited(const MouseEvent&) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDragged(const MouseEvent&) override {}
    void mouseUp(const MouseEvent&) override {}

private:
    std::vector<std::string> items_;
    int selected_;
    bool enabled_ = true, hover_ = false;
};

/// A non-editable line of text.
class Label : public View {
public:
    Label(std::string text = "", Font font = Fonts::body, Color color = Theme::text);
    void setText(const std::string& s);
    const std::string& text() const { return text_; }
    void setColor(const Color& c);
    Align align = Align::Left;
    /// Wrap onto several lines instead of truncating.
    bool wraps = false;
    Font font;

    void draw(Gfx& g, const Rect& dirty) override;
    View* hitTest(Point) override { return nullptr; }

private:
    std::string text_;
    Color color_;
};

/// A horizontal slider with tick marks; the value snaps to whole ticks.
class Slider : public View {
public:
    Slider(int ticks, int value) : ticks_(ticks), value_(value) {}
    void setValue(int v);
    int value() const { return value_; }
    std::function<void(int)> onChange;

    void draw(Gfx& g, const Rect& dirty) override;
    void mouseDown(const MouseEvent& e) override;
    void mouseDragged(const MouseEvent& e) override;
    void mouseUp(const MouseEvent&) override {}

private:
    int ticks_;
    int value_;
    void track(const MouseEvent& e);
};

}  // namespace ui
