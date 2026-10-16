// uishot: renders the main window offscreen to a PNG, after running a script
// of commands. Used to check layouts (and simple interactions) without a screen.
//
//   uishot OUT.png [command…]
//
// Commands: example:N  station:N  cycles:N  end  select:LINE  opt:0|1  regs:N  snapshot:N
//           caches:0|1  fwd:0|1  branch:N  pred:N  source:TEXT (\n for newlines)
//           size:WxH  scale:S  click:X,Y  dblclick:X,Y  move:X,Y  drag:X1,Y1,X2,Y2  wheel:X,Y,DY
//           type:TEXT  key:NAME (enter tab back del left right up down home end pgup pgdn esc f3,
//           with ctrl+ / shift+ prefixes)  cmd:undo|redo|copy|paste|selectAll|find  tooltip  bench:N

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <cstdio>
#include <map>

#include "../ui/HostBase.h"
#include "../ui/Theme.h"
#include "../ui/Views.h"

using namespace ui;

namespace {

/// Forwards the model's timers to the host once it exists.
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

class OffscreenHost : public HostBase {
public:
    using HostBase::HostBase;
    void invalidate() override {}
    void schedule(int id, double, std::function<void()> f) override { pending[id] = std::move(f); }
    void repeat(int, double, std::function<void()>) override {}
    void cancel(int id) override { pending.erase(id); }
    /// Runs the timers that would have fired (such as the compile scheduled after typing).
    void flush() {
        while (!pending.empty()) {
            auto f = pending.begin()->second;
            pending.erase(pending.begin());
            f();
        }
    }
    std::map<int, std::function<void()>> pending;
};

Com<IWICImagingFactory> wicFactory() {
    Com<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)wic.put());
    return wic;
}

/// Renders the view tree into a new bitmap.
Com<IWICBitmap> render(OffscreenHost& host, Size size, float scale) {
    Com<IWICImagingFactory> wic = wicFactory();
    Com<IWICBitmap> bmp;
    wic->CreateBitmap((UINT)(size.w * scale), (UINT)(size.h * scale), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.put());
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    Com<ID2D1RenderTarget> rt;
    if (FAILED(d2dFactory()->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put()))) {
        printf("could not create a render target\n");
        return bmp;
    }
    rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    Gfx gfx(d2dFactory());
    gfx.begin(rt.get());
    host.renderTree(gfx, size);
    gfx.end();
    return bmp;
}

bool savePNG(IWICBitmap* bmp, const std::wstring& path) {
    Com<IWICImagingFactory> wic = wicFactory();
    Com<IWICStream> stream;
    wic->CreateStream(stream.put());
    if (FAILED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) return false;
    Com<IWICBitmapEncoder> enc;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put());
    enc->Initialize(stream.get(), WICBitmapEncoderNoCache);
    Com<IWICBitmapFrameEncode> frame;
    enc->CreateNewFrame(frame.put(), nullptr);
    frame->Initialize(nullptr);
    UINT w, h;
    bmp->GetSize(&w, &h);
    frame->SetSize(w, h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppPBGRA;
    frame->SetPixelFormat(&fmt);
    frame->WriteSource(bmp, nullptr);
    frame->Commit();
    return SUCCEEDED(enc->Commit());
}

int vkFor(const std::string& name) {
    static const std::map<std::string, int> keys = {
        {"enter", VK_RETURN}, {"tab", VK_TAB},   {"back", VK_BACK}, {"del", VK_DELETE}, {"left", VK_LEFT}, {"right", VK_RIGHT},
        {"up", VK_UP},        {"down", VK_DOWN}, {"home", VK_HOME}, {"end", VK_END},    {"pgup", VK_PRIOR}, {"pgdn", VK_NEXT},
        {"esc", VK_ESCAPE},   {"f3", VK_F3},     {"f5", VK_F5},
    };
    auto it = keys.find(name);
    if (it != keys.end()) return it->second;
    if (name.size() == 1) return toupper((unsigned char)name[0]);
    return 0;
}

std::vector<float> numbers(const std::string& s) {
    std::vector<float> out;
    for (auto& p : sp::split(s, ',')) out.push_back(std::strtof(p.c_str(), nullptr));
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (argc < 2) {
        printf("usage: uishot out.png [example:N] [station:N] [cycles:N] ...\n");
        return 1;
    }
    std::wstring out = argv[1];
    SchedulerProxy proxy;
    AppModel model(&proxy, false);
    MainView main(model);
    OffscreenHost host(&main);
    proxy.target = &host;
    Size size{1480, 920};
    float scale = 1;
    main.setFrame({0, 0, size.w, size.h});
    main.layoutSubtreeIfNeeded();
    main.start();
    host.flush();
    Point mouse;
    for (int k = 2; k < argc; k++) {
        std::string cmd = narrow(argv[k]);
        size_t colon = cmd.find(':');
        std::string name = cmd.substr(0, colon);
        std::string arg = colon == std::string::npos ? "" : cmd.substr(colon + 1);
        int n = (int)sp::parseInt(arg).value_or(0);
        // Interactions need the layout (and the rects views compute while drawing) to be current.
        render(host, size, 1);
        if (name == "example") model.load(sp::Examples::all().at(n));
        else if (name == "station") model.setStation((sp::Station)n);
        else if (name == "cycles") model.stepCycle(n);
        else if (name == "end") model.runToEnd();
        else if (name == "select") model.selectLine(n, sp::Station::Source);
        else if (name == "opt") {
            sp::CompileOptions o = model.options();
            o.passes = n == 0 ? std::set<sp::PassID>{} : std::set<sp::PassID>(sp::allPasses().begin(), sp::allPasses().end());
            model.setOptions(o);
            model.compileNow();
        } else if (name == "regs") {
            sp::CompileOptions o = model.options();
            o.registerLimit = n == 0 ? std::nullopt : std::optional<int>(n);
            model.setOptions(o);
            model.compileNow();
        } else if (name == "snapshot") model.setIRSnapshot(n);
        else if (name == "caches") {
            sp::PipelineConfig c = model.pipelineConfig();
            c.hierarchy.enabled = n == 1;
            model.setPipelineConfig(c);
        } else if (name == "fwd") {
            sp::PipelineConfig c = model.pipelineConfig();
            c.forwarding = n == 1;
            model.setPipelineConfig(c);
        } else if (name == "branch") {
            sp::PipelineConfig c = model.pipelineConfig();
            c.branchStage = sp::allBranchStages().at(n);
            model.setPipelineConfig(c);
        } else if (name == "pred") {
            sp::PipelineConfig c = model.pipelineConfig();
            c.predictor = sp::allPredictors().at(n);
            model.setPipelineConfig(c);
        } else if (name == "source") {
            model.setSource(sp::replaceAll(arg, "\\n", "\n"), false);
            model.compileNow();
        } else if (name == "size") {
            auto v = sp::split(arg, 'x');
            if (v.size() == 2) size = {std::strtof(v[0].c_str(), nullptr), std::strtof(v[1].c_str(), nullptr)};
        } else if (name == "scale") scale = std::strtof(arg.c_str(), nullptr);
        else if (name == "click" || name == "dblclick") {
            auto v = numbers(arg);
            mouse = {v.at(0), v.at(1)};
            host.mouseMove(mouse, false);
            host.mouseDown(mouse, 1, false, false, false);
            host.mouseUp(mouse);
            if (name == "dblclick") {
                host.mouseDown(mouse, 2, false, false, false);
                host.mouseUp(mouse);
            }
        } else if (name == "move") {
            auto v = numbers(arg);
            mouse = {v.at(0), v.at(1)};
            host.mouseMove(mouse, false);
        } else if (name == "drag") {
            auto v = numbers(arg);
            host.mouseMove({v.at(0), v.at(1)}, false);
            host.mouseDown({v.at(0), v.at(1)}, 1, false, false, false);
            for (int s = 1; s <= 8; s++) host.mouseMove({v[0] + (v[2] - v[0]) * s / 8, v[1] + (v[3] - v[1]) * s / 8}, true);
            host.mouseUp({v.at(2), v.at(3)});
        } else if (name == "wheel") {
            auto v = numbers(arg);
            host.wheel({v.at(0), v.at(1)}, 0, v.at(2), false);
        } else if (name == "type") {
            std::wstring w = widen(sp::replaceAll(arg, "\\n", "\n"));
            for (wchar_t c : w) {
                if (c == L'\n') host.key(VK_RETURN, false, false, false);
                else host.character(c);
            }
        } else if (name == "key") {
            bool ctrl = false, shift = false;
            std::string kname = arg;
            while (true) {
                if (sp::hasPrefix(kname, "ctrl+")) {
                    ctrl = true;
                    kname = kname.substr(5);
                } else if (sp::hasPrefix(kname, "shift+")) {
                    shift = true;
                    kname = kname.substr(6);
                } else {
                    break;
                }
            }
            host.key(vkFor(kname), shift, ctrl, false);
        } else if (name == "cmd") {
            if (!host.editCommand(arg) && arg == "find") main.editor.showFind();
        } else if (name == "bench") {
            // Times N frames of playback (one cycle per frame) into one software render target.
            Com<IWICImagingFactory> wic = wicFactory();
            Com<IWICBitmap> bmp;
            wic->CreateBitmap((UINT)(size.w * scale), (UINT)(size.h * scale), GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.put());
            D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
                D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
            Com<ID2D1RenderTarget> rt;
            d2dFactory()->CreateWicBitmapRenderTarget(bmp.get(), props, rt.put());
            Gfx gfx(d2dFactory());
            double total = 0, worst = 0;
            for (int f = 0; f < n; f++) {
                model.stepCycle(1);
                host.flush();
                LARGE_INTEGER t0, t1, freq;
                QueryPerformanceFrequency(&freq);
                QueryPerformanceCounter(&t0);
                gfx.begin(rt.get());
                host.renderTree(gfx, size);
                gfx.end();
                QueryPerformanceCounter(&t1);
                double ms = 1000.0 * (t1.QuadPart - t0.QuadPart) / freq.QuadPart;
                total += ms;
                worst = std::max(worst, ms);
            }
            printf("bench: %d frames, %.2f ms average, %.2f ms worst\n", n, n ? total / n : 0, worst);
        } else if (name == "tooltip") {
            host.tooltipText = host.tooltipAt(mouse);
            host.tooltipPoint = mouse;
        } else {
            printf("unknown command %s\n", cmd.c_str());
        }
        host.flush();
    }
    Com<IWICBitmap> bmp = render(host, size, scale);
    if (!savePNG(bmp.get(), out)) {
        printf("could not write %s\n", narrow(out).c_str());
        return 1;
    }
    printf("wrote %s\n", narrow(out).c_str());
    return 0;
}
