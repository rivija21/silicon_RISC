#include "Gfx.h"

#include <cstdio>

namespace ui {

// MARK: - Strings

std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

ID2D1Factory* d2dFactory() {
    static Com<ID2D1Factory> f = [] {
        Com<ID2D1Factory> x;
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void**)x.put());
        return x;
    }();
    return f.get();
}

// MARK: - Attributed strings

void AttrString::add(const std::string& utf8, const Font& f, const Color& c, std::optional<Color> background) {
    addWide(widen(utf8), f, c, background);
}

void AttrString::addWide(const std::wstring& s, const Font& f, const Color& c, std::optional<Color> background) {
    if (s.empty()) return;
    key_.clear();
    int start = (int)text_.size();
    text_ += s;
    if (!runs_.empty()) {
        TextRun& last = runs_.back();
        if (last.font == f && last.color == c && last.background.has_value() == background.has_value() &&
            (!background || *last.background == *background) && last.start + last.length == start) {
            last.length += (int)s.size();
            return;
        }
    }
    runs_.push_back({start, (int)s.size(), f, c, background});
}

Size AttrString::size() const { return TextEngine::get().measure(*this); }

const std::string& AttrString::cacheKey() const {
    if (!key_.empty() || text_.empty()) return key_;
    key_.assign(reinterpret_cast<const char*>(text_.data()), text_.size() * sizeof(wchar_t));
    key_ += '\x01';
    for (auto& r : runs_) {
        uint64_t font = r.font.key();
        uint32_t color = r.color.key();
        key_.append(reinterpret_cast<const char*>(&r.start), sizeof r.start);
        key_.append(reinterpret_cast<const char*>(&r.length), sizeof r.length);
        key_.append(reinterpret_cast<const char*>(&font), sizeof font);
        key_.append(reinterpret_cast<const char*>(&color), sizeof color);
    }
    return key_;
}

// MARK: - Colour effects and the text renderer

namespace {

/// Carries a colour through DirectWrite to our text renderer.
class ColorEffect final : public IUnknown {
public:
    explicit ColorEffect(const Color& c) : color(c) {}
    Color color;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }

private:
    ULONG refs = 1;
};

/// Draws glyph runs with the colour carried by each run's drawing effect.
class TextRenderer final : public IDWriteTextRenderer {
public:
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    Color defaultColor;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) || riid == __uuidof(IDWriteTextRenderer)) {
            *out = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --refs;
        if (r == 0) delete this;
        return r;
    }

    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*, BOOL* disabled) override {
        *disabled = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*, DWRITE_MATRIX* m) override {
        D2D1_MATRIX_3X2_F t;
        rt->GetTransform(&t);
        m->m11 = t._11;
        m->m12 = t._12;
        m->m21 = t._21;
        m->m22 = t._22;
        m->dx = t._31;
        m->dy = t._32;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* ppd) override {
        float dx, dy;
        rt->GetDpi(&dx, &dy);
        *ppd = dx / 96.0f;
        return S_OK;
    }
    Color colorOf(IUnknown* effect) const { return effect ? static_cast<ColorEffect*>(effect)->color : defaultColor; }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE mode, const DWRITE_GLYPH_RUN* run,
                                           const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown* effect) override {
        brush->SetColor(d2d(colorOf(effect)));
        rt->DrawGlyphRun(D2D1::Point2F(x, y), run, brush, mode);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*, FLOAT x, FLOAT y, const DWRITE_UNDERLINE* u, IUnknown* effect) override {
        brush->SetColor(d2d(colorOf(effect)));
        rt->FillRectangle(D2D1::RectF(x, y + u->offset, x + u->width, y + u->offset + u->thickness), brush);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*, FLOAT x, FLOAT y, const DWRITE_STRIKETHROUGH* s, IUnknown* effect) override {
        brush->SetColor(d2d(colorOf(effect)));
        rt->FillRectangle(D2D1::RectF(x, y + s->offset, x + s->width, y + s->offset + s->thickness), brush);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void* ctx, FLOAT x, FLOAT y, IDWriteInlineObject* obj, BOOL sideways, BOOL rtl,
                                               IUnknown* effect) override {
        return obj->Draw(ctx, this, x, y, sideways, rtl, effect);
    }

private:
    ULONG refs = 1;
};

DWRITE_FONT_WEIGHT dwWeight(Weight w) {
    switch (w) {
    case Weight::Regular: return DWRITE_FONT_WEIGHT_NORMAL;
    case Weight::Medium: return DWRITE_FONT_WEIGHT_MEDIUM;
    case Weight::Semibold: return DWRITE_FONT_WEIGHT_SEMI_BOLD;
    case Weight::Bold: return DWRITE_FONT_WEIGHT_BOLD;
    }
    return DWRITE_FONT_WEIGHT_NORMAL;
}

}  // namespace

// MARK: - Text engine

TextEngine& TextEngine::get() {
    static TextEngine engine;
    return engine;
}

TextEngine::TextEngine() {
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)factory_.put());
    Com<IDWriteFontCollection> fonts;
    factory_->GetSystemFontCollection(fonts.put(), FALSE);
    auto pick = [&](std::initializer_list<const wchar_t*> names) -> std::wstring {
        for (const wchar_t* n : names) {
            UINT32 index;
            BOOL exists = FALSE;
            if (fonts && SUCCEEDED(fonts->FindFamilyName(n, &index, &exists)) && exists) return n;
        }
        return *names.begin();
    };
    families_[(int)Family::UI] = pick({L"Segoe UI", L"Tahoma", L"Arial"});
    families_[(int)Family::Serif] = pick({L"Cambria", L"Georgia", L"Times New Roman"});
    families_[(int)Family::Mono] = pick({L"Cascadia Mono", L"Consolas", L"Courier New"});
}

IDWriteTextFormat* TextEngine::format(const Font& f) {
    auto it = formats_.find(f.key());
    if (it != formats_.end()) return it->second.get();
    Com<IDWriteTextFormat> fmt;
    factory_->CreateTextFormat(families_[(int)f.family].c_str(), nullptr, dwWeight(f.weight),
                               f.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, f.size, L"en-us",
                               fmt.put());
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteTextFormat* raw = fmt.get();
    formats_[f.key()] = std::move(fmt);
    return raw;
}

float TextEngine::lineHeight(const Font& f) {
    auto it = lineHeights_.find(f.key());
    if (it != lineHeights_.end()) return it->second;
    Com<IDWriteTextLayout> l;
    factory_->CreateTextLayout(L"Ag", 2, format(f), 1e6f, 1e6f, l.put());
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    lineHeights_[f.key()] = m.height;
    return m.height;
}

IUnknown* TextEngine::colorEffect(const Color& c) {
    auto it = effects_.find(c.key());
    if (it != effects_.end()) return it->second.get();
    Com<IUnknown> e(new ColorEffect(c));
    IUnknown* raw = e.get();
    effects_[c.key()] = std::move(e);
    return raw;
}

IDWriteTextLayout* TextEngine::store(const std::string& key, IDWriteTextLayout* l) {
    if (cache_.size() > 8000) {
        cache_.clear();
        metrics_.clear();
    }
    Com<IDWriteTextLayout> c(l);
    cache_[key] = c;
    return l;
}

void TextEngine::applyBox(IDWriteTextLayout* l, IDWriteTextFormat* fmt, const Font& f, const TextBox& box) {
    l->SetWordWrapping(box.wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    l->SetTextAlignment(box.align == Align::Left ? DWRITE_TEXT_ALIGNMENT_LEADING
                        : box.align == Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                     : DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (box.truncate) {
        Com<IDWriteInlineObject> sign;
        factory_->CreateEllipsisTrimmingSign(fmt, sign.put());
        DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        l->SetTrimming(&t, sign.get());
    }
    if (box.lineSpacing > 0) {
        Com<IDWriteTextLayout> probe;
        factory_->CreateTextLayout(L"Ag", 2, fmt, 1e6f, 1e6f, probe.put());
        DWRITE_LINE_METRICS lm{};
        UINT32 count = 0;
        probe->GetLineMetrics(&lm, 1, &count);
        l->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, lm.height + box.lineSpacing, lm.baseline);
    }
    (void)f;
}

namespace {

template <typename T>
void appendBytes(std::string& key, const T& v) {
    key.append(reinterpret_cast<const char*>(&v), sizeof v);
}

/// The binary prefix of a layout cache key: a tag, the font and the box options.
std::string boxKey(char tag, uint64_t font, const TextBox& box) {
    std::string key(1, tag);
    key.reserve(64);
    appendBytes(key, font);
    appendBytes(key, box.maxWidth);
    appendBytes(key, box.maxHeight);
    appendBytes(key, box.lineSpacing);
    key += (char)box.align;
    key += (char)((box.wrap ? 1 : 0) | (box.truncate ? 2 : 0));
    return key;
}

}  // namespace

IDWriteTextLayout* TextEngine::create(const std::wstring& text, const Font& f, const TextBox& box) {
    IDWriteTextFormat* fmt = format(f);
    IDWriteTextLayout* l = nullptr;
    if (FAILED(factory_->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt, std::max(0.0f, box.maxWidth), std::max(0.0f, box.maxHeight), &l)))
        return nullptr;
    applyBox(l, fmt, f, box);
    return l;
}

IDWriteTextLayout* TextEngine::layout(const std::wstring& text, const Font& f, const TextBox& box) {
    std::string key = boxKey('\x02', f.key(), box);
    key.append(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();
    IDWriteTextLayout* l = create(text, f, box);
    return l ? store(key, l) : nullptr;
}

IDWriteTextLayout* TextEngine::layout(const std::string& utf8, const Font& f, const TextBox& box) {
    // Keyed on the UTF-8 bytes, so a cache hit needs no conversion.
    std::string key = boxKey('\x01', f.key(), box);
    key += utf8;
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();
    IDWriteTextLayout* l = create(widen(utf8), f, box);
    return l ? store(key, l) : nullptr;
}

IDWriteTextLayout* TextEngine::layout(const AttrString& a, const TextBox& box) {
    std::string key = boxKey('\x03', 0, box);
    key += a.cacheKey();
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();
    Font base = a.baseFont();
    IDWriteTextFormat* fmt = format(base);
    IDWriteTextLayout* l = nullptr;
    if (FAILED(factory_->CreateTextLayout(a.text().c_str(), (UINT32)a.text().size(), fmt, std::max(0.0f, box.maxWidth),
                                          std::max(0.0f, box.maxHeight), &l)))
        return nullptr;
    for (auto& r : a.runs()) {
        DWRITE_TEXT_RANGE range{(UINT32)r.start, (UINT32)r.length};
        if (!(r.font == base)) {
            l->SetFontFamilyName(families_[(int)r.font.family].c_str(), range);
            l->SetFontSize(r.font.size, range);
            l->SetFontWeight(dwWeight(r.font.weight), range);
            l->SetFontStyle(r.font.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, range);
        }
        l->SetDrawingEffect(colorEffect(r.color), range);
    }
    applyBox(l, fmt, base, box);
    return store(key, l);
}

Size TextEngine::metrics(IDWriteTextLayout* l) {
    if (!l) return {0, 0};
    auto it = metrics_.find(l);
    if (it != metrics_.end()) return it->second;
    DWRITE_TEXT_METRICS m{};
    l->GetMetrics(&m);
    Size s{m.widthIncludingTrailingWhitespace, m.height};
    metrics_[l] = s;
    return s;
}

Size TextEngine::measure(const std::wstring& text, const Font& f) {
    if (text.empty()) return {0, lineHeight(f)};
    return metrics(layout(text, f, TextBox()));
}

Size TextEngine::measure(const std::string& utf8, const Font& f) {
    if (utf8.empty()) return {0, lineHeight(f)};
    return metrics(layout(utf8, f, TextBox()));
}

Size TextEngine::measure(const AttrString& a) {
    if (a.empty()) return {0, 0};
    return metrics(layout(a, TextBox()));
}

Size textSize(const std::string& s, const Font& f) { return TextEngine::get().measure(s, f); }

// MARK: - Gfx

Gfx::~Gfx() {
    if (renderer_) renderer_->Release();
}

void Gfx::begin(ID2D1RenderTarget* rt) {
    if (rt != rt_) {
        brush_.reset();
        strokes_.clear();
    }
    rt_ = rt;
    if (!brush_) rt_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), brush_.put());
    if (!renderer_) renderer_ = new TextRenderer();
    auto* tr = static_cast<TextRenderer*>(renderer_);
    tr->rt = rt_;
    tr->brush = brush_.get();
    rt_->BeginDraw();
    rt_->SetTransform(D2D1::Matrix3x2F::Identity());
}

bool Gfx::end() {
    HRESULT hr = rt_->EndDraw();
    layers_.clear();
    return hr != (HRESULT)D2DERR_RECREATE_TARGET;
}

void Gfx::releaseDeviceResources() {
    brush_.reset();
    strokes_.clear();
    layers_.clear();
    rt_ = nullptr;
}

ID2D1SolidColorBrush* Gfx::brush(const Color& c) {
    brush_->SetColor(d2d(c));
    return brush_.get();
}

ID2D1StrokeStyle* Gfx::stroke(const std::vector<float>& dash) {
    if (dash.empty()) return nullptr;
    auto it = strokes_.find(dash);
    if (it != strokes_.end()) return it->second.get();
    // dash[0] holds the line width: Direct2D dash lengths are in multiples of it.
    std::vector<float> units;
    for (size_t k = 1; k < dash.size(); k++) units.push_back(dash[k] / std::max(0.1f, dash[0]));
    Com<ID2D1StrokeStyle> s;
    factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                            D2D1_LINE_JOIN_MITER, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f),
                                units.data(), (UINT32)units.size(), s.put());
    ID2D1StrokeStyle* raw = s.get();
    strokes_[dash] = std::move(s);
    return raw;
}

void Gfx::fillRect(const Rect& r, const Color& c) {
    if (c.isClear() || r.isEmpty()) return;
    rt_->FillRectangle(d2d(r), brush(c));
}

void Gfx::strokeRect(const Rect& r, const Color& c, float width) {
    if (c.isClear()) return;
    rt_->DrawRectangle(d2d(r), brush(c), width);
}

void Gfx::fillRounded(const Rect& r, float radius, const Color& c) {
    if (c.isClear() || r.w <= 0 || r.h <= 0) return;
    float rad = std::min({radius, r.w / 2, r.h / 2});
    rt_->FillRoundedRectangle(D2D1::RoundedRect(d2d(r), rad, rad), brush(c));
}

void Gfx::strokeRounded(const Rect& r0, float radius, const Color& c, float width) {
    if (c.isClear()) return;
    Rect r = r0.insetBy(width / 2, width / 2);
    if (r.w <= 0 || r.h <= 0) return;
    float rad = std::min({radius, r.w / 2, r.h / 2});
    rt_->DrawRoundedRectangle(D2D1::RoundedRect(d2d(r), rad, rad), brush(c), width);
}

void Gfx::fillEllipse(const Rect& r, const Color& c) {
    if (c.isClear()) return;
    rt_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(r.midX(), r.midY()), r.w / 2, r.h / 2), brush(c));
}

void Gfx::line(Point a, Point b, const Color& c, float width, const std::vector<float>& dash) {
    if (c.isClear()) return;
    ID2D1StrokeStyle* s = nullptr;
    if (!dash.empty()) {
        std::vector<float> key{width};
        key.insert(key.end(), dash.begin(), dash.end());
        s = stroke(key);
    }
    rt_->DrawLine(d2d(a), d2d(b), brush(c), width, s);
}

ID2D1PathGeometry* Gfx::path(const std::vector<Point>& pts, bool closed, bool filled) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(factory_->CreatePathGeometry(&g))) return nullptr;
    Com<ID2D1GeometrySink> sink;
    g->Open(sink.put());
    sink->BeginFigure(d2d(pts[0]), filled ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
    for (size_t k = 1; k < pts.size(); k++) sink->AddLine(d2d(pts[k]));
    sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
    sink->Close();
    return g;
}

void Gfx::fillPolygon(const std::vector<Point>& pts, const Color& c) {
    if (pts.size() < 3 || c.isClear()) return;
    Com<ID2D1PathGeometry> g(path(pts, true, true));
    if (g) rt_->FillGeometry(g.get(), brush(c));
}

void Gfx::strokePolyline(const std::vector<Point>& pts, const Color& c, float width, const std::vector<float>& dash, bool closed) {
    if (pts.size() < 2 || c.isClear()) return;
    Com<ID2D1PathGeometry> g(path(pts, closed, false));
    ID2D1StrokeStyle* s = nullptr;
    if (!dash.empty()) {
        std::vector<float> key{width};
        key.insert(key.end(), dash.begin(), dash.end());
        s = stroke(key);
    }
    if (g) rt_->DrawGeometry(g.get(), brush(c), width, s);
}

void Gfx::drawLayout(IDWriteTextLayout* l, Point p, const Color& defaultColor) {
    if (!l) return;
    auto* tr = static_cast<TextRenderer*>(renderer_);
    tr->defaultColor = defaultColor;
    l->Draw(nullptr, tr, p.x, p.y);
}

void Gfx::drawText(const std::string& s, Point p, const Font& f, const Color& c) {
    if (s.empty() || c.isClear()) return;
    drawLayout(TextEngine::get().layout(s, f, TextBox()), p, c);
}

void Gfx::drawText(const std::string& s, const Rect& r, const Font& f, const Color& c, Align align) {
    if (s.empty() || c.isClear() || r.w <= 0) return;
    TextBox box;
    box.maxWidth = r.w;
    box.align = align;
    box.truncate = true;
    TextEngine& te = TextEngine::get();
    float h = te.lineHeight(f);
    drawLayout(te.layout(s, f, box), {r.x, r.midY() - h / 2}, c);
}

void Gfx::drawAttributed(const AttrString& a, Point p) {
    if (a.empty()) return;
    IDWriteTextLayout* l = TextEngine::get().layout(a, TextBox());
    if (!l) return;
    for (auto& run : a.runs()) {
        if (!run.background) continue;
        UINT32 count = 0;
        DWRITE_HIT_TEST_METRICS m[8];
        if (SUCCEEDED(l->HitTestTextRange(run.start, run.length, p.x, p.y, m, 8, &count)))
            for (UINT32 k = 0; k < count; k++) fillRect({m[k].left, m[k].top, m[k].width, m[k].height}, *run.background);
    }
    drawLayout(l, p, Color::black());
}

void Gfx::drawAttributed(const AttrString& a, const Rect& r, TextBox box) {
    if (a.empty() || r.w <= 0) return;
    box.maxWidth = r.w;
    box.maxHeight = r.h;
    IDWriteTextLayout* l = TextEngine::get().layout(a, box);
    if (!l) return;
    pushClip(r);
    for (auto& run : a.runs()) {
        if (!run.background) continue;
        UINT32 count = 0;
        DWRITE_HIT_TEST_METRICS m[8];
        if (SUCCEEDED(l->HitTestTextRange(run.start, run.length, r.x, r.y, m, 8, &count)))
            for (UINT32 k = 0; k < count; k++) fillRect({m[k].left, m[k].top, m[k].width, m[k].height}, *run.background);
    }
    drawLayout(l, r.origin(), Color::black());
    popClip();
}

void Gfx::drawTextBox(const std::string& s, const Rect& r, const Font& f, const Color& c, TextBox box) {
    if (s.empty() || r.w <= 0) return;
    box.maxWidth = r.w;
    box.maxHeight = r.h;
    IDWriteTextLayout* l = TextEngine::get().layout(s, f, box);
    pushClip(r);
    drawLayout(l, r.origin(), c);
    popClip();
}

void Gfx::pushClip(const Rect& r) { rt_->PushAxisAlignedClip(d2d(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Gfx::popClip() { rt_->PopAxisAlignedClip(); }

void Gfx::pushRoundedClip(const Rect& r, float radius) {
    Com<ID2D1RoundedRectangleGeometry> g;
    factory_->CreateRoundedRectangleGeometry(D2D1::RoundedRect(d2d(r), radius, radius), g.put());
    Com<ID2D1Layer> layer;
    rt_->CreateLayer(nullptr, layer.put());
    rt_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), layer.get());
    layers_.push_back(layer);
}

void Gfx::popRoundedClip() {
    rt_->PopLayer();
    if (!layers_.empty()) layers_.pop_back();
}

void Gfx::translate(float dx, float dy) {
    D2D1_MATRIX_3X2_F m;
    rt_->GetTransform(&m);
    m._31 += dx * m._11 + dy * m._21;
    m._32 += dx * m._12 + dy * m._22;
    rt_->SetTransform(m);
}

D2D1_MATRIX_3X2_F Gfx::transform() const {
    D2D1_MATRIX_3X2_F m;
    rt_->GetTransform(&m);
    return m;
}

void Gfx::setTransform(const D2D1_MATRIX_3X2_F& m) { rt_->SetTransform(m); }

// MARK: - Icons

void Gfx::drawIcon(Icon icon, const Rect& r, const Color& c) {
    // Icons are drawn in a 16 × 16 box centred in r.
    float s = std::min(r.w, r.h) / 16.0f;
    float ox = r.midX() - 8 * s, oy = r.midY() - 8 * s;
    auto P = [&](float x, float y) { return Point(ox + x * s, oy + y * s); };
    auto tri = [&](float x, float y, float w, float h) { fillPolygon({P(x, y), P(x + w, y + h / 2), P(x, y + h)}, c); };
    auto triL = [&](float x, float y, float w, float h) { fillPolygon({P(x + w, y), P(x, y + h / 2), P(x + w, y + h)}, c); };
    auto bar = [&](float x, float y, float w, float h) { fillRounded({ox + x * s, oy + y * s, w * s, h * s}, 0.6f * s, c); };
    switch (icon) {
    case Icon::Play: tri(4, 2.5f, 9.5f, 11); break;
    case Icon::Pause:
        bar(4, 3, 3, 10);
        bar(9, 3, 3, 10);
        break;
    case Icon::StepCycle:
        tri(3.5f, 3, 7, 10);
        bar(11, 3, 2, 10);
        break;
    case Icon::StepBack:
        bar(3, 3, 2, 10);
        triL(5.5f, 3, 7, 10);
        break;
    case Icon::StepInstruction:
        tri(1.5f, 3.5f, 6.5f, 9);
        tri(8, 3.5f, 6.5f, 9);
        break;
    case Icon::RunToEnd:
        tri(1, 3.5f, 6, 9);
        tri(6.5f, 3.5f, 6, 9);
        bar(12.5f, 3.5f, 2, 9);
        break;
    case Icon::StepLine:
        tri(1.5f, 3.5f, 5, 7);
        line(P(8, 4), P(15, 4), c, 1.6f * s);
        line(P(8, 7.5f), P(15, 7.5f), c, 1.6f * s);
        line(P(1.5f, 12), P(15, 12), c, 1.6f * s);
        break;
    case Icon::Reset: {
        // A circular arrow, counter-clockwise.
        std::vector<Point> arc;
        float cx = 8, cy = 8.5f, rad = 5;
        for (int k = 0; k <= 20; k++) {
            float a = (-60 + k * 14.5f) * 3.14159265f / 180.0f;
            arc.push_back(P(cx + rad * std::cos(a), cy + rad * std::sin(a)));
        }
        strokePolyline(arc, c, 1.7f * s);
        Point tip = arc.front();
        fillPolygon({Point(tip.x - 0.5f * s, tip.y - 3.6f * s), Point(tip.x + 3.6f * s, tip.y + 0.6f * s), Point(tip.x - 1.4f * s, tip.y + 1.8f * s)}, c);
        break;
    }
    case Icon::Book: {
        strokePolyline({P(8, 4), P(5, 3), P(1.5f, 3), P(1.5f, 13), P(5, 13), P(8, 14), P(11, 13), P(14.5f, 13), P(14.5f, 3), P(11, 3), P(8, 4)}, c,
                       1.3f * s, {}, true);
        line(P(8, 4), P(8, 14), c, 1.3f * s);
        break;
    }
    case Icon::ExpandAll:
        strokePolyline({P(4, 3.5f), P(8, 7.5f), P(12, 3.5f)}, c, 1.7f * s);
        strokePolyline({P(4, 8.5f), P(8, 12.5f), P(12, 8.5f)}, c, 1.7f * s);
        break;
    case Icon::CollapseAll:
        strokePolyline({P(4, 7.5f), P(8, 3.5f), P(12, 7.5f)}, c, 1.7f * s);
        strokePolyline({P(4, 12.5f), P(8, 8.5f), P(12, 12.5f)}, c, 1.7f * s);
        break;
    case Icon::Warning:
        fillPolygon({P(8, 1), P(15.5f, 14.5f), P(0.5f, 14.5f)}, c);
        fillRounded({ox + 7.1f * s, oy + 5 * s, 1.8f * s, 5.3f * s}, 0.9f * s, Color::white());
        fillEllipse({ox + 7.1f * s, oy + 11.3f * s, 1.8f * s, 1.8f * s}, Color::white());
        break;
    }
}

}  // namespace ui
