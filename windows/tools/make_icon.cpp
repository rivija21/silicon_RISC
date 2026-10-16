// Draws the app icon (the same design as scripts/make_icon.swift) and writes res/app.ico.
// Usage: build/make_icon.exe res/app.ico

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

template <class T>
struct Ptr {
    T* p = nullptr;
    ~Ptr() {
        if (p) p->Release();
    }
    T* operator->() const { return p; }
    T** put() { return &p; }
};

static D2D1_COLOR_F rgb(unsigned v, float a = 1) { return D2D1::ColorF(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a); }

/// Renders the icon at `px` × `px` and returns it as PNG bytes.
static std::vector<char> renderPNG(ID2D1Factory* d2d, IDWriteFactory* dw, IWICImagingFactory* wic, int px) {
    Ptr<IWICBitmap> bmp;
    wic->CreateBitmap(px, px, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, bmp.put());
    float s = px / 1024.0f;
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    Ptr<ID2D1RenderTarget> rt;
    d2d->CreateWicBitmapRenderTarget(bmp.p, props, rt.put());
    Ptr<ID2D1SolidColorBrush> brush;
    rt->CreateSolidColorBrush(rgb(0), brush.put());
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    rt->SetTransform(D2D1::Matrix3x2F::Scale(s, s));
    auto rr = [](float x, float y, float w, float h, float r) { return D2D1::RoundedRect(D2D1::RectF(x, y, x + w, y + h), r, r); };

    // Parchment card with a soft shadow below it.
    for (int k = 6; k >= 1; k--) {
        brush->SetColor(rgb(0x3A2A10, 0.05f));
        rt->FillRoundedRectangle(rr(100 - k * 3, 100 + 10 - k * 2, 824 + k * 6, 824 + k * 6, 185 + k * 3), brush.p);
    }
    Ptr<ID2D1RoundedRectangleGeometry> card;
    d2d->CreateRoundedRectangleGeometry(rr(100, 100, 824, 824, 185), card.put());
    Ptr<ID2D1Layer> layer;
    rt->CreateLayer(nullptr, layer.put());
    rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), card.p), layer.p);
    D2D1_GRADIENT_STOP stops[2] = {{0, rgb(0xFFFDF6)}, {1, rgb(0xF1E7D3)}};
    Ptr<ID2D1GradientStopCollection> sc;
    rt->CreateGradientStopCollection(stops, 2, sc.put());
    Ptr<ID2D1LinearGradientBrush> grad;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(D2D1::Point2F(512, 100), D2D1::Point2F(512, 924)), sc.p, grad.put());
    rt->FillRectangle(D2D1::RectF(100, 100, 924, 924), grad.p);
    brush->SetColor(rgb(0xDCE6D8));
    for (float v = 100; v <= 924.5f; v += 41.2f) {
        rt->DrawLine(D2D1::Point2F(v, 100), D2D1::Point2F(v, 924), brush.p, 2);
        rt->DrawLine(D2D1::Point2F(100, v), D2D1::Point2F(924, v), brush.p, 2);
    }
    // The pipeline staircase: four instructions flowing through five stages.
    const unsigned stages[5] = {0x2F6FC9, 0x2A8F62, 0xD27A22, 0x8552B8, 0xBF3D5A};
    float cw = 78, step = 84, ch = 50;
    float x0 = 512 - (8 * step - (step - cw)) / 2;
    for (int row = 0; row < 4; row++)
        for (int k = 0; k < 5; k++) {
            brush->SetColor(rgb(stages[k], 0.4f + 0.2f * row));
            rt->FillRoundedRectangle(rr(x0 + (row + k) * step, 224 + row * 62, cw, ch, 10), brush.p);
        }
    // A chip with pins, holding a "C".
    float cx = 382, cy = 574, cs = 260;
    brush->SetColor(rgb(0x1F4E9E));
    for (int k = 0; k < 5; k++) {
        float o = cx + 33 + k * 46;
        rt->FillRectangle(D2D1::RectF(o, cy + cs - 4, o + 18, cy + cs + 34), brush.p);
        rt->FillRectangle(D2D1::RectF(o, cy - 34, o + 18, cy + 4), brush.p);
        float oy = cy + cs - 33 - k * 46 - 18;
        rt->FillRectangle(D2D1::RectF(cx - 34, oy, cx + 4, oy + 18), brush.p);
        rt->FillRectangle(D2D1::RectF(cx + cs - 4, oy, cx + cs + 34, oy + 18), brush.p);
    }
    rt->FillRoundedRectangle(rr(cx, cy, cs, cs, 34), brush.p);
    brush->SetColor(D2D1::ColorF(1, 1, 1, 0.25f));
    rt->DrawRoundedRectangle(rr(cx + 22, cy + 22, cs - 44, cs - 44, 20), brush.p, 6);
    rt->PopLayer();
    // The "C".
    Ptr<IDWriteTextFormat> fmt;
    dw->CreateTextFormat(L"Cambria", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 190, L"en-us", fmt.put());
    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
    rt->DrawText(L"C", 1, fmt.p, D2D1::RectF(cx, cy, cx + cs, cy + cs), brush.p);
    rt->EndDraw();

    // Encode as PNG in memory.
    IStream* mem = nullptr;
    CreateStreamOnHGlobal(nullptr, TRUE, &mem);
    Ptr<IWICBitmapEncoder> enc;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, enc.put());
    enc->Initialize(mem, WICBitmapEncoderNoCache);
    Ptr<IWICBitmapFrameEncode> frame;
    enc->CreateNewFrame(frame.put(), nullptr);
    frame->Initialize(nullptr);
    frame->SetSize(px, px);
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppPBGRA;
    frame->SetPixelFormat(&pf);
    frame->WriteSource(bmp.p, nullptr);
    frame->Commit();
    enc->Commit();
    STATSTG st{};
    mem->Stat(&st, STATFLAG_NONAME);
    std::vector<char> bytes((size_t)st.cbSize.QuadPart);
    LARGE_INTEGER zero{};
    mem->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    mem->Read(bytes.data(), (ULONG)bytes.size(), &read);
    mem->Release();
    return bytes;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: make_icon out.ico\n");
        return 1;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Ptr<ID2D1Factory> d2d;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)d2d.put());
    Ptr<IDWriteFactory> dw;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)dw.put());
    Ptr<IWICImagingFactory> wic;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, __uuidof(IWICImagingFactory), (void**)wic.put());
    const int sizes[] = {16, 20, 24, 32, 40, 48, 64, 96, 128, 256};
    std::vector<std::vector<char>> images;
    for (int px : sizes) images.push_back(renderPNG(d2d.p, dw.p, wic.p, px));
    std::ofstream f(std::filesystem::path(argv[1]), std::ios::binary);
    auto u16 = [&](unsigned v) { f.put((char)(v & 255)).put((char)(v >> 8)); };
    auto u32 = [&](unsigned v) { u16(v & 0xFFFF); u16(v >> 16); };
    int n = (int)images.size();
    u16(0);
    u16(1);
    u16(n);
    unsigned offset = 6 + 16 * n;
    for (int k = 0; k < n; k++) {
        f.put((char)(sizes[k] >= 256 ? 0 : sizes[k]));
        f.put((char)(sizes[k] >= 256 ? 0 : sizes[k]));
        f.put(0);
        f.put(0);
        u16(1);
        u16(32);
        u32((unsigned)images[k].size());
        u32(offset);
        offset += (unsigned)images[k].size();
    }
    for (auto& img : images) f.write(img.data(), img.size());
    printf("wrote %ls (%d sizes)\n", argv[1], n);
    return 0;
}
