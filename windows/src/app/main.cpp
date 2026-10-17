// Silicon Path for Windows: the application — window, menus, keyboard shortcuts, files.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <filesystem>
#include <fstream>

#include "../ui/Views.h"
#include "../ui/Window.h"

using namespace ui;

namespace {

enum Command {
    CmdNew = 100, CmdOpen, CmdSaveAs, CmdExportAsm, CmdExit,
    CmdUndo = 200, CmdRedo, CmdCut, CmdCopy, CmdPaste, CmdSelectAll, CmdFind, CmdFindNext,
    CmdRunPause = 300, CmdStepCycle, CmdStepInstruction, CmdStepLine, CmdStepBack, CmdRunToEnd, CmdReset, CmdCompile,
    CmdStation = 400,   // 400…409
    CmdExample = 500,   // 500…
    CmdAbout = 600,
};

/// Forwards the model's timers to the window once it exists.
struct SchedulerProxy : Scheduler {
    Scheduler* target = nullptr;
    void schedule(int id, double s, std::function<void()> f) override {
        if (target) target->schedule(id, s, std::move(f));
    }
    void repeat(int id, double s, std::function<void()> f) override {
        if (target) target->repeat(id, s, std::move(f));
    }
    void cancel(int id) override {
        if (target) target->cancel(id);
    }
};

HMENU buildMenu() {
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, CmdNew, L"&New C Program\tCtrl+N");
    AppendMenuW(file, MF_STRING, CmdOpen, L"&Open…\tCtrl+O");
    AppendMenuW(file, MF_STRING, CmdSaveAs, L"&Save As…\tCtrl+S");
    AppendMenuW(file, MF_STRING, CmdExportAsm, L"&Export Assembly…\tCtrl+E");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    HMENU ex = CreatePopupMenu();
    for (int k = 0; k < (int)sp::Examples::all().size(); k++) AppendMenuW(ex, MF_STRING, CmdExample + k, widen(sp::Examples::all()[k].name).c_str());
    AppendMenuW(file, MF_POPUP, (UINT_PTR)ex, L"E&xamples");
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, CmdExit, L"E&xit\tAlt+F4");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, CmdUndo, L"&Undo\tCtrl+Z");
    AppendMenuW(edit, MF_STRING, CmdRedo, L"&Redo\tCtrl+Y");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, CmdCut, L"Cu&t\tCtrl+X");
    AppendMenuW(edit, MF_STRING, CmdCopy, L"&Copy\tCtrl+C");
    AppendMenuW(edit, MF_STRING, CmdPaste, L"&Paste\tCtrl+V");
    AppendMenuW(edit, MF_STRING, CmdSelectAll, L"Select &All\tCtrl+A");
    AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(edit, MF_STRING, CmdFind, L"&Find…\tCtrl+F");
    AppendMenuW(edit, MF_STRING, CmdFindNext, L"Find &Next\tF3");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");

    HMENU run = CreatePopupMenu();
    AppendMenuW(run, MF_STRING, CmdRunPause, L"&Run / Pause\tF5");
    AppendMenuW(run, MF_STRING, CmdStepCycle, L"Step One &Cycle\tCtrl+J");
    AppendMenuW(run, MF_STRING, CmdStepInstruction, L"Step One &Instruction\tCtrl+K");
    AppendMenuW(run, MF_STRING, CmdStepLine, L"Step One C &Line\tCtrl+L");
    AppendMenuW(run, MF_STRING, CmdStepBack, L"Step &Back One Cycle\tCtrl+Shift+J");
    AppendMenuW(run, MF_STRING, CmdRunToEnd, L"Run to the &End\tShift+F5");
    AppendMenuW(run, MF_STRING, CmdReset, L"Re&set Machine\tCtrl+Shift+R");
    AppendMenuW(run, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(run, MF_STRING, CmdCompile, L"C&ompile Now\tCtrl+B");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)run, L"&Run");

    HMENU view = CreatePopupMenu();
    for (int k = 0; k < sp::stationCount; k++) {
        std::wstring label = widen(sp::cat(k + 1, ". ", sp::stationTitle((sp::Station)k))) + (k < 9 ? L"\tCtrl+" + std::to_wstring(k + 1) : L"\tCtrl+0");
        AppendMenuW(view, MF_STRING, CmdStation + k, label.c_str());
    }
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");

    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, CmdAbout, L"&About Silicon Path");
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
    return bar;
}

HACCEL buildAccelerators() {
    std::vector<ACCEL> a = {
        {FVIRTKEY | FCONTROL, 'N', CmdNew},
        {FVIRTKEY | FCONTROL, 'O', CmdOpen},
        {FVIRTKEY | FCONTROL, 'S', CmdSaveAs},
        {FVIRTKEY | FCONTROL, 'E', CmdExportAsm},
        {FVIRTKEY | FCONTROL, 'Z', CmdUndo},
        {FVIRTKEY | FCONTROL, 'Y', CmdRedo},
        {FVIRTKEY | FCONTROL | FSHIFT, 'Z', CmdRedo},
        {FVIRTKEY | FCONTROL, 'X', CmdCut},
        {FVIRTKEY | FCONTROL, 'C', CmdCopy},
        {FVIRTKEY | FCONTROL, 'V', CmdPaste},
        {FVIRTKEY | FCONTROL, 'A', CmdSelectAll},
        {FVIRTKEY | FCONTROL, 'F', CmdFind},
        {FVIRTKEY, VK_F3, CmdFindNext},
        {FVIRTKEY, VK_F5, CmdRunPause},
        {FVIRTKEY | FCONTROL, 'R', CmdRunPause},
        {FVIRTKEY | FCONTROL, 'J', CmdStepCycle},
        {FVIRTKEY | FCONTROL, 'K', CmdStepInstruction},
        {FVIRTKEY | FCONTROL, 'L', CmdStepLine},
        {FVIRTKEY | FCONTROL | FSHIFT, 'J', CmdStepBack},
        {FVIRTKEY | FSHIFT, VK_F5, CmdRunToEnd},
        {FVIRTKEY | FCONTROL | FSHIFT, 'R', CmdReset},
        {FVIRTKEY | FCONTROL, 'B', CmdCompile},
    };
    for (int k = 0; k < sp::stationCount; k++) a.push_back({FVIRTKEY | FCONTROL, (WORD)(k < 9 ? '1' + k : '0'), (WORD)(CmdStation + k)});
    return CreateAcceleratorTableW(a.data(), (int)a.size());
}

std::wstring fileDialog(HWND owner, bool save, const std::wstring& defaultName, const wchar_t* filter, const wchar_t* defExt) {
    wchar_t path[MAX_PATH * 4] = {};
    wcsncpy(path, defaultName.c_str(), MAX_PATH * 4 - 1);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = path;
    ofn.nMaxFile = MAX_PATH * 4;
    ofn.lpstrDefExt = defExt;
    ofn.Flags = save ? (OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR) : (OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR);
    BOOL ok = save ? GetSaveFileNameW(&ofn) : GetOpenFileNameW(&ofn);
    return ok ? std::wstring(path) : std::wstring();
}

bool writeText(const std::wstring& path, const std::string& text) {
    // Windows line endings, UTF-8 without a byte-order mark.
    std::string crlf;
    for (char c : text) {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    std::ofstream f(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
    f << crlf;
    return (bool)f;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int cmdShow) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    SchedulerProxy proxy;
    AppModel model(&proxy);
    MainView main(model);
    HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    Window window(&main, L"Silicon Path", icon);
    proxy.target = &window;
    window.setMenu(buildMenu());
    HACCEL accel = buildAccelerators();
    if (auto p = Settings::shared().string("window")) window.restorePlacement(*p);

    HWND hwnd = window.hwnd();
    const wchar_t* sourceFilter = L"C or assembly source (*.c;*.s;*.asm)\0*.c;*.s;*.asm\0All files (*.*)\0*.*\0";
    window.onCommand = [&](int cmd) {
        switch (cmd) {
        case CmdNew:
            model.load(sp::Example{"Untitled", "", "int main() {\n    printf(\"hello, RISC-V\\n\");\n    return 0;\n}\n", false, sp::Station::Source});
            break;
        case CmdOpen: {
            std::wstring p = fileDialog(hwnd, false, L"", sourceFilter, L"c");
            if (!p.empty()) model.loadFile(p);
            break;
        }
        case CmdSaveAs: {
            std::wstring p = fileDialog(hwnd, true, model.isAssembly() ? L"program.s" : L"program.c", sourceFilter, model.isAssembly() ? L"s" : L"c");
            if (!p.empty() && !writeText(p, main.editor.text())) MessageBoxW(hwnd, L"The file could not be saved.", L"Silicon Path", MB_ICONWARNING);
            break;
        }
        case CmdExportAsm: {
            auto c = model.shown();
            if (!c) break;
            std::wstring p = fileDialog(hwnd, true, L"program.s", L"Assembly (*.s)\0*.s\0All files (*.*)\0*.*\0", L"s");
            if (p.empty()) break;
            std::string text;
            for (auto& l : c->asmLines) text += l.text + "\n";
            if (!writeText(p, text)) MessageBoxW(hwnd, L"The file could not be saved.", L"Silicon Path", MB_ICONWARNING);
            break;
        }
        case CmdExit: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
        case CmdUndo: window.editCommand("undo"); break;
        case CmdRedo: window.editCommand("redo"); break;
        case CmdCut: window.editCommand("cut"); break;
        case CmdCopy: window.editCommand("copy"); break;
        case CmdPaste: window.editCommand("paste"); break;
        case CmdSelectAll: window.editCommand("selectAll"); break;
        case CmdFind:
            if (!window.editCommand("find")) main.editor.showFind();
            break;
        case CmdFindNext: main.editor.find(1); break;
        case CmdRunPause: model.togglePlay(); break;
        case CmdStepCycle: model.stepCycle(); break;
        case CmdStepInstruction: model.stepInstruction(); break;
        case CmdStepLine: model.stepLine(); break;
        case CmdStepBack: model.stepBack(); break;
        case CmdRunToEnd: model.runToEnd(); break;
        case CmdReset: model.resetMachine(); break;
        case CmdCompile: model.compileNow(); break;
        case CmdAbout:
            MessageBoxW(hwnd,
                        L"Follow a C program all the way down: tokens, syntax tree, symbols, IR and optimisation, RISC-V assembly, machine code "
                        L"and linking, then a 5-stage pipeline with caches.\n\nEverything here (compiler, assembler, linker, simulators) is "
                        L"implemented from scratch, following Patterson & Hennessy's RISC-V edition.",
                        L"Silicon Path", MB_ICONINFORMATION);
            break;
        default:
            if (cmd >= CmdStation && cmd < CmdStation + sp::stationCount) model.setStation((sp::Station)(cmd - CmdStation));
            else if (cmd >= CmdExample && cmd < CmdExample + (int)sp::Examples::all().size()) model.load(sp::Examples::all()[cmd - CmdExample]);
        }
    };
    window.onDropFile = [&](const std::wstring& path) { model.loadFile(path); };
    window.onClose = [&] {
        model.pause();
        model.save();
        Settings::shared().set("window", window.placement());
        Settings::shared().save();
    };

    window.show(cmdShow);
    main.start();
    window.setFocus(&main.editor.textView);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc > 1) model.loadFile(argv[1]);
    LocalFree(argv);
    int r = window.run(accel);
    DestroyAcceleratorTable(accel);
    return r;
}
