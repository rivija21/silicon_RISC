// Event routing shared by the real window and the offscreen renderer:
// hit testing, hover (entered/exited), mouse capture, keyboard focus, tooltips.
#pragma once

#include "AppModel.h"
#include "View.h"

namespace ui {

class HostBase : public Host, public Scheduler {
public:
    explicit HostBase(View* root);
    ~HostBase() override { root_->setHost(nullptr); }
    View* root() const { return root_; }

    // Host
    void setFocus(View* v) override;
    View* focus() const override { return focus_; }
    void forget(View* v) override;

    // Input, in window coordinates (DIPs).
    void mouseDown(Point p, int clickCount, bool shift, bool ctrl, bool alt, bool right = false);
    void mouseMove(Point p, bool buttonDown);
    void mouseUp(Point p);
    void mouseLeave();
    void wheel(Point p, float dx, float dy, bool shift);
    void key(int vk, bool shift, bool ctrl, bool alt);
    void character(wchar_t c);
    /// Sends an edit command to the focused view and up its superviews; returns true if one handled it.
    bool editCommand(const std::string& cmd);
    Cursor cursorAt(Point p);
    std::string tooltipAt(Point p);

    /// Lays out and draws the tree (and the tooltip) on a surface of the given size.
    void renderTree(Gfx& g, Size size);
    std::string tooltipText;
    Point tooltipPoint;

protected:
    View* root_;
    View* focus_ = nullptr;
    View* captured_ = nullptr;
    std::vector<View*> hoverChain_;
    MouseEvent makeEvent(Point p, bool shift = false, bool ctrl = false, bool alt = false) const;
    void updateHover(Point p);
};

}  // namespace ui
