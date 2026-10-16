#include "Window.h"

#include <dwmapi.h>
#include <imm.h>
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>

#include "Theme.h"

namespace ui {

static const wchar_t* kClassName = L"SiliconPathWindow";
static const int kTooltipTimer = 9000;

/// Set SILICONPATH_LOG to a file path to log paints (for checking the app on machines without a screen).
static void debugLog(const char* fmt, ...) {
    static FILE* f = [] {
        wchar_t path[MAX_PATH];
        return GetEnvironmentVariableW(L"SILICONPATH_LOG", path, MAX_PATH) ? _wfopen(path, L"a") : nullptr;
    }();
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

Window::Window(View* root, const std::wstring& title, HICON icon) : HostBase(root), gfx_(d2dFactory()) {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = proc;
    wc.hInstance = inst;
    wc.hIcon = icon;
    wc.hIconSm = icon;
    wc.hCursor = nullptr;
    wc.lpszClassName = kClassName;
    RegisterClassExW(&wc);

    // Size: as large as the original's window, but never larger than the screen.
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    HDC screen = GetDC(nullptr);
    float sysScale = GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, screen);
    int workW = work.right - work.left, workH = work.bottom - work.top;
    int w = std::min((int)(1520 * sysScale), workW - (int)(20 * sysScale));
    int h = std::min((int)(960 * sysScale), workH - (int)(10 * sysScale));
    int x = work.left + (workW - w) / 2, y = work.top + (workH - h) / 2;
    hwnd_ = CreateWindowExW(0, kClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y, w, h, nullptr, nullptr, inst, this);
    dpi_ = (float)GetDpiForWindow(hwnd_);
    DragAcceptFiles(hwnd_, TRUE);
    // Windows 11: a title bar in the app's ivory, with dark text.
    COLORREF caption = RGB(0xFF, 0xFD, 0xF8), text = RGB(0x2B, 0x2A, 0x33);
    DwmSetWindowAttribute(hwnd_, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof caption);
    DwmSetWindowAttribute(hwnd_, 36 /* DWMWA_TEXT_COLOR */, &text, sizeof text);
}

Window::~Window() {
    if (hwnd_) DestroyWindow(hwnd_);
}

void Window::show(int cmdShow) {
    ShowWindow(hwnd_, cmdShow);
    UpdateWindow(hwnd_);
}

void Window::invalidate() {
    if (hwnd_ && !paintPending_) {
        paintPending_ = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

POINT Window::toScreen(Point p) const {
    POINT pt{(LONG)std::lround(p.x * scale()), (LONG)std::lround(p.y * scale())};
    ClientToScreen(hwnd_, &pt);
    return pt;
}

int Window::popupMenu(const std::vector<std::string>& items, int checked, Point at) {
    HMENU m = CreatePopupMenu();
    for (int k = 0; k < (int)items.size(); k++)
        AppendMenuW(m, MF_STRING | (k == checked ? MF_CHECKED : 0), k + 1, widen(items[k]).c_str());
    buttonDown_ = false;
    captured_ = nullptr;
    ReleaseCapture();
    POINT pt = toScreen(at);
    int r = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(m);
    return r - 1;
}

void Window::schedule(int id, double seconds, std::function<void()> f) {
    timers_[id] = Timer{std::move(f), false};
    SetTimer(hwnd_, id, std::max(1u, (UINT)(seconds * 1000)), nullptr);
}

void Window::repeat(int id, double seconds, std::function<void()> f) {
    timers_[id] = Timer{std::move(f), true};
    SetTimer(hwnd_, id, std::max(1u, (UINT)(seconds * 1000)), nullptr);
}

void Window::cancel(int id) {
    KillTimer(hwnd_, id);
    timers_.erase(id);
}

int Window::run(HACCEL accel) {
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (accel && TranslateAcceleratorW(hwnd_, accel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}

std::string Window::placement() const {
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    GetWindowPlacement(hwnd_, &wp);
    RECT r = wp.rcNormalPosition;
    char buf[96];
    snprintf(buf, sizeof buf, "%ld,%ld,%ld,%ld,%d", r.left, r.top, r.right - r.left, r.bottom - r.top, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0);
    return buf;
}

void Window::restorePlacement(const std::string& s) {
    long x, y, w, h;
    int max;
    if (sscanf(s.c_str(), "%ld,%ld,%ld,%ld,%d", &x, &y, &w, &h, &max) != 5 || w < 200 || h < 200) return;
    RECT r{x, y, x + w, y + h};
    if (!MonitorFromRect(&r, MONITOR_DEFAULTTONULL)) return;
    WINDOWPLACEMENT wp{};
    wp.length = sizeof wp;
    wp.rcNormalPosition = r;
    wp.showCmd = max ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    SetWindowPlacement(hwnd_, &wp);
}

bool Window::ensureTarget() {
    if (rt_) return true;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(), dpi_, dpi_);
    // Present without blocking on vsync; DWM composes the window anyway, and the message loop stays responsive.
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp = D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right - rc.left, rc.bottom - rc.top),
                                                                             D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    if (FAILED(d2dFactory()->CreateHwndRenderTarget(props, hp, rt_.put()))) return false;
    rt_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
    return true;
}

void Window::paint() {
    paintPending_ = false;
    if (!ensureTarget()) return;
    RECT rc;
    GetClientRect(hwnd_, &rc);
    static int paints = 0;
    LARGE_INTEGER t0, t1, freq;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    gfx_.begin(rt_.get());
    renderTree(gfx_, {(rc.right - rc.left) / scale(), (rc.bottom - rc.top) / scale()});
    bool ok = gfx_.end();
    QueryPerformanceCounter(&t1);
    debugLog("paint %d: %ldx%ld px, dpi %.0f, %.1f ms, %s", ++paints, rc.right - rc.left, rc.bottom - rc.top, dpi_,
             1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart, ok ? "ok" : "device lost");
    if (!ok) {
        gfx_.releaseDeviceResources();
        rt_.reset();
        invalidate();
    }
}

void Window::updateImeWindow() {
    if (!focus_) return;
    auto r = focus_->caretRect();
    if (!r) return;
    HIMC imc = ImmGetContext(hwnd_);
    if (!imc) return;
    COMPOSITIONFORM cf{};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos = POINT{(LONG)(r->x * scale()), (LONG)(r->y * scale())};
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM cand{};
    cand.dwStyle = CFS_CANDIDATEPOS;
    cand.ptCurrentPos = POINT{(LONG)(r->x * scale()), (LONG)(r->maxY() * scale())};
    ImmSetCandidateWindow(imc, &cand);
    ImmReleaseContext(hwnd_, imc);
}

LRESULT CALLBACK Window::proc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(l);
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        reinterpret_cast<Window*>(cs->lpCreateParams)->hwnd_ = h;
    }
    auto* self = reinterpret_cast<Window*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self) return DefWindowProcW(h, msg, w, l);
    return self->handle(msg, w, l);
}

LRESULT Window::handle(UINT msg, WPARAM w, LPARAM l) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd_, &ps);
        paint();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE:
        if (rt_) rt_->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        invalidate();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(l);
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        float s = hwnd_ ? scale() : 1;
        mm->ptMinTrackSize.x = std::min((LONG)(1180 * s), (LONG)((work.right - work.left) * 0.95));
        mm->ptMinTrackSize.y = std::min((LONG)(720 * s), (LONG)((work.bottom - work.top) * 0.95));
        return 0;
    }
    case WM_DPICHANGED: {
        dpi_ = (float)HIWORD(w);
        auto* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(hwnd_, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        if (rt_) rt_->SetDpi(dpi_, dpi_);
        invalidate();
        return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        SetCapture(hwnd_);
        buttonDown_ = true;
        POINT pt{GET_X_LPARAM_(l), GET_Y_LPARAM_(l)};
        DWORD now = GetMessageTime();
        int dx = GetSystemMetrics(SM_CXDOUBLECLK) / 2, dy = GetSystemMetrics(SM_CYDOUBLECLK) / 2;
        if (now - lastClickTime_ <= GetDoubleClickTime() && std::abs(pt.x - lastClickPos_.x) <= dx && std::abs(pt.y - lastClickPos_.y) <= dy)
            clickCount_ += 1;
        else
            clickCount_ = 1;
        lastClickTime_ = now;
        lastClickPos_ = pt;
        KillTimer(hwnd_, kTooltipTimer);
        tooltipText.clear();
        mouseDown(dip(l), clickCount_, (w & MK_SHIFT) != 0, (w & MK_CONTROL) != 0, GetKeyState(VK_MENU) < 0);
        updateImeWindow();
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!trackingLeave_) {
            TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd_, 0};
            TrackMouseEvent(&t);
            trackingLeave_ = true;
        }
        Point p = dip(l);
        bool moved = std::abs(p.x - lastMouse_.x) > 1 || std::abs(p.y - lastMouse_.y) > 1;
        lastMouse_ = p;
        mouseMove(p, buttonDown_);
        if (moved && !buttonDown_) {
            if (!tooltipText.empty()) {
                tooltipText.clear();
                invalidate();
            }
            SetTimer(hwnd_, kTooltipTimer, 650, nullptr);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (buttonDown_) {
            buttonDown_ = false;
            ReleaseCapture();
            mouseUp(dip(l));
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (buttonDown_ && (HWND)l != hwnd_) {
            buttonDown_ = false;
            mouseUp(lastMouse_);
        }
        return 0;
    case WM_MOUSELEAVE:
        trackingLeave_ = false;
        KillTimer(hwnd_, kTooltipTimer);
        mouseLeave();
        return 0;
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL: {
        POINT pt{GET_X_LPARAM_(l), GET_Y_LPARAM_(l)};
        ScreenToClient(hwnd_, &pt);
        float delta = (float)(short)HIWORD(w) / WHEEL_DELTA;
        UINT lines = 3;
        SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        float step = std::max(1u, std::min(lines, 10u)) * 17.0f;
        Point p{pt.x / scale(), pt.y / scale()};
        if (msg == WM_MOUSEWHEEL) wheel(p, 0, -delta * step, (GET_KEYSTATE_WPARAM(w) & MK_SHIFT) != 0);
        else wheel(p, delta * step, 0, false);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd_, &pt);
            Cursor c = cursorAt({pt.x / scale(), pt.y / scale()});
            LPCWSTR id = c == Cursor::IBeam ? IDC_IBEAM : c == Cursor::ResizeLeftRight ? IDC_SIZEWE : c == Cursor::ResizeUpDown ? IDC_SIZENS
                         : c == Cursor::Hand ? IDC_HAND : IDC_ARROW;
            SetCursor(LoadCursorW(nullptr, id));
            return TRUE;
        }
        break;
    case WM_KEYDOWN:
        key((int)w, GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0);
        updateImeWindow();
        return 0;
    case WM_CHAR:
        character((wchar_t)w);
        updateImeWindow();
        return 0;
    case WM_IME_STARTCOMPOSITION: updateImeWindow(); break;
    case WM_TIMER: {
        int id = (int)w;
        if (id == kTooltipTimer) {
            KillTimer(hwnd_, kTooltipTimer);
            std::string t = tooltipAt(lastMouse_);
            if (!t.empty()) {
                tooltipText = t;
                tooltipPoint = lastMouse_;
                invalidate();
            }
            return 0;
        }
        auto it = timers_.find(id);
        if (it == timers_.end()) {
            KillTimer(hwnd_, id);
            return 0;
        }
        std::function<void()> f = it->second.f;
        if (!it->second.repeating) {
            KillTimer(hwnd_, id);
            timers_.erase(it);
        }
        f();
        return 0;
    }
    case WM_COMMAND:
        if (HIWORD(w) == 0 || HIWORD(w) == 1) {
            if (onCommand) onCommand(LOWORD(w));
            return 0;
        }
        break;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)w;
        wchar_t path[MAX_PATH * 4];
        if (DragQueryFileW(drop, 0, path, MAX_PATH * 4) && onDropFile) onDropFile(path);
        DragFinish(drop);
        return 0;
    }
    case WM_ACTIVATE: invalidate(); break;
    case WM_CLOSE:
        if (onClose) onClose();
        DestroyWindow(hwnd_);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    case WM_NCDESTROY: {
        HWND h = hwnd_;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        hwnd_ = nullptr;
        return DefWindowProcW(h, msg, w, l);
    }
    }
    return DefWindowProcW(hwnd_, msg, w, l);
}

}  // namespace ui
