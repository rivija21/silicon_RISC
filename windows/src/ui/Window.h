// The Win32 window: a Direct2D surface hosting a view tree, with mouse,
// keyboard, DPI, timers, tooltips, menus and file drops.
#pragma once

#include <map>

#include "HostBase.h"

namespace ui {

class Window : public HostBase {
public:
    Window(View* root, const std::wstring& title, HICON icon);
    ~Window() override;
    HWND hwnd() const override { return hwnd_; }
    void show(int cmdShow);
    float scale() const { return dpi_ / 96.0f; }

    // Host
    void invalidate() override;
    int popupMenu(const std::vector<std::string>& items, int checked, Point at) override;
    POINT toScreen(Point p) const override;

    // Scheduler
    void schedule(int id, double seconds, std::function<void()> f) override;
    void repeat(int id, double seconds, std::function<void()> f) override;
    void cancel(int id) override;

    std::function<void(int)> onCommand;
    std::function<void(const std::wstring&)> onDropFile;
    std::function<void()> onClose;
    void setMenu(HMENU m) { SetMenu(hwnd_, m); }
    /// Runs the message loop with the given accelerator table.
    int run(HACCEL accel);
    /// Saved placement ("x,y,w,h,maximized"), for the settings file.
    std::string placement() const;
    void restorePlacement(const std::string& s);

private:
    HWND hwnd_ = nullptr;
    float dpi_ = 96;
    Com<ID2D1HwndRenderTarget> rt_;
    Gfx gfx_;
    bool trackingLeave_ = false;
    bool buttonDown_ = false;
    DWORD lastClickTime_ = 0;
    POINT lastClickPos_{};
    int clickCount_ = 0;
    Point lastMouse_;
    struct Timer {
        std::function<void()> f;
        bool repeating;
    };
    std::map<int, Timer> timers_;
    bool paintPending_ = false;

    static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM w, LPARAM l);
    LRESULT handle(UINT msg, WPARAM w, LPARAM l);
    void paint();
    bool ensureTarget();
    Point dip(LPARAM l) const { return {GET_X_LPARAM_(l) / scale(), GET_Y_LPARAM_(l) / scale()}; }
    static int GET_X_LPARAM_(LPARAM l) { return (int)(short)LOWORD(l); }
    static int GET_Y_LPARAM_(LPARAM l) { return (int)(short)HIWORD(l); }
    void updateImeWindow();
};

}  // namespace ui
