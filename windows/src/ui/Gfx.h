// Drawing with Direct2D and DirectWrite: shapes, text (plain and attributed),
// a cache of text layouts, and a few vector icons.
#pragma once

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "Geometry.h"

namespace ui {

/// A minimal COM smart pointer.
template <class T>
class Com {
public:
    Com() = default;
    Com(T* p) : p_(p) {}
    Com(const Com& o) : p_(o.p_) {
        if (p_) p_->AddRef();
    }
    Com(Com&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    ~Com() { reset(); }
    Com& operator=(const Com& o) {
        if (this != &o) {
            if (o.p_) o.p_->AddRef();
            reset();
            p_ = o.p_;
        }
        return *this;
    }
    Com& operator=(Com&& o) noexcept {
        if (this != &o) {
            reset();
            p_ = o.p_;
            o.p_ = nullptr;
        }
        return *this;
    }
    void reset() {
        if (p_) p_->Release();
        p_ = nullptr;
    }
    T* get() const { return p_; }
    T* operator->() const { return p_; }
    T** put() {
        reset();
        return &p_;
    }
    explicit operator bool() const { return p_ != nullptr; }

private:
    T* p_ = nullptr;
};

// MARK: - Fonts and text

enum class Weight { Regular, Medium, Semibold, Bold };
enum class Family { UI, Serif, Mono };
enum class Align { Left, Center, Right };

struct Font {
    Family family = Family::UI;
    float size = 12;
    Weight weight = Weight::Regular;
    bool italic = false;
    bool operator==(const Font&) const = default;
    Font withWeight(Weight w) const { Font f = *this; f.weight = w; return f; }
    Font italicized() const { Font f = *this; f.italic = true; return f; }
    uint64_t key() const {
        return (uint64_t)family << 40 | (uint64_t)weight << 36 | (uint64_t)italic << 32 | (uint32_t)std::lround(size * 100);
    }
};

/// One styled run inside an attributed string (UTF-16 offsets).
struct TextRun {
    int start = 0;
    int length = 0;
    Font font;
    Color color;
    std::optional<Color> background;
};

/// Text with per-run fonts and colours, like NSAttributedString.
class AttrString {
public:
    AttrString() = default;
    AttrString(const std::string& utf8, const Font& f, const Color& c) { add(utf8, f, c); }
    void add(const std::string& utf8, const Font& f, const Color& c, std::optional<Color> background = std::nullopt);
    void addWide(const std::wstring& s, const Font& f, const Color& c, std::optional<Color> background = std::nullopt);
    const std::wstring& text() const { return text_; }
    const std::vector<TextRun>& runs() const { return runs_; }
    int length() const { return (int)text_.size(); }
    bool empty() const { return text_.empty(); }
    /// The font of the first run (used as the layout's base font).
    Font baseFont() const { return runs_.empty() ? Font() : runs_[0].font; }
    Size size() const;
    const std::string& cacheKey() const;

private:
    std::wstring text_;
    std::vector<TextRun> runs_;
    mutable std::string key_;   // cached cacheKey()
};

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

/// Options for laying text out in a box.
struct TextBox {
    float maxWidth = 1e6f;
    float maxHeight = 1e6f;
    Align align = Align::Left;
    bool wrap = false;
    bool truncate = false;
    float lineSpacing = 0;   // extra space between lines
};

/// DirectWrite factory, text formats, and a cache of text layouts.
class TextEngine {
public:
    static TextEngine& get();
    IDWriteFactory* factory() const { return factory_.get(); }
    IDWriteTextFormat* format(const Font& f);
    /// A cached layout for plain text.
    IDWriteTextLayout* layout(const std::wstring& text, const Font& f, const TextBox& box);
    IDWriteTextLayout* layout(const std::string& utf8, const Font& f, const TextBox& box);
    /// A cached layout for attributed text (colours are drawing effects).
    IDWriteTextLayout* layout(const AttrString& a, const TextBox& box);
    /// Width (including trailing spaces) and line height.
    Size measure(const std::wstring& text, const Font& f);
    Size measure(const std::string& utf8, const Font& f);
    Size measure(const AttrString& a);
    /// Width and height of a layout (cached alongside it).
    Size metrics(IDWriteTextLayout* l);
    float lineHeight(const Font& f);
    const std::wstring& familyName(Family f) const { return families_[(int)f]; }
    /// The drawing effect that carries a colour to the text renderer.
    IUnknown* colorEffect(const Color& c);
    void clearCache() {
        cache_.clear();
        metrics_.clear();
    }

private:
    TextEngine();
    Com<IDWriteFactory> factory_;
    std::wstring families_[3];
    std::unordered_map<uint64_t, Com<IDWriteTextFormat>> formats_;
    std::unordered_map<std::string, Com<IDWriteTextLayout>> cache_;
    std::unordered_map<IDWriteTextLayout*, Size> metrics_;
    std::unordered_map<uint32_t, Com<IUnknown>> effects_;
    std::unordered_map<uint64_t, float> lineHeights_;
    Com<IDWriteInlineObject> ellipsis_;
    IDWriteTextLayout* store(const std::string& key, IDWriteTextLayout* l);
    IDWriteTextLayout* create(const std::wstring& text, const Font& f, const TextBox& box);
    void applyBox(IDWriteTextLayout* l, IDWriteTextFormat* fmt, const Font& f, const TextBox& box);
};

/// Width and line height of a string in a font.
Size textSize(const std::string& s, const Font& f);

// MARK: - Icons

enum class Icon { Reset, StepBack, StepCycle, StepInstruction, StepLine, Play, Pause, RunToEnd, Book, ExpandAll, CollapseAll, Warning };

// MARK: - Drawing

/// A drawing surface (a window or an offscreen bitmap) wrapping a Direct2D render target.
class Gfx {
public:
    explicit Gfx(ID2D1Factory* factory) : factory_(factory) {}
    ~Gfx();
    void begin(ID2D1RenderTarget* rt);
    /// Returns false if the render target must be recreated.
    bool end();
    ID2D1RenderTarget* rt() const { return rt_; }
    ID2D1Factory* factory() const { return factory_; }
    void releaseDeviceResources();

    void fillRect(const Rect& r, const Color& c);
    void strokeRect(const Rect& r, const Color& c, float width = 1);
    void fillRounded(const Rect& r, float radius, const Color& c);
    /// Strokes inside the rectangle (inset by half the width), like the original helpers.
    void strokeRounded(const Rect& r, float radius, const Color& c, float width = 1);
    void fillEllipse(const Rect& r, const Color& c);
    void line(Point a, Point b, const Color& c, float width = 1, const std::vector<float>& dash = {});
    void fillPolygon(const std::vector<Point>& pts, const Color& c);
    void strokePolyline(const std::vector<Point>& pts, const Color& c, float width = 1, const std::vector<float>& dash = {}, bool closed = false);

    /// Text with its top-left corner at p.
    void drawText(const std::string& s, Point p, const Font& f, const Color& c);
    /// Single-line text fitted to the rect, centred vertically, truncated with an ellipsis.
    void drawText(const std::string& s, const Rect& r, const Font& f, const Color& c, Align align = Align::Left);
    void drawAttributed(const AttrString& a, Point p);
    /// Attributed text in a box: wrapped and/or truncated as the box says.
    void drawAttributed(const AttrString& a, const Rect& r, TextBox box);
    /// Plain text in a box (multi-line).
    void drawTextBox(const std::string& s, const Rect& r, const Font& f, const Color& c, TextBox box);
    void drawLayout(IDWriteTextLayout* l, Point p, const Color& defaultColor);

    void drawIcon(Icon icon, const Rect& r, const Color& c);

    void pushClip(const Rect& r);
    void popClip();
    /// A rounded clip (anti-aliased), for cards that mask their children.
    void pushRoundedClip(const Rect& r, float radius);
    void popRoundedClip();
    void translate(float dx, float dy);
    D2D1_MATRIX_3X2_F transform() const;
    void setTransform(const D2D1_MATRIX_3X2_F& m);

private:
    ID2D1Factory* factory_;
    ID2D1RenderTarget* rt_ = nullptr;
    Com<ID2D1SolidColorBrush> brush_;
    std::map<std::vector<float>, Com<ID2D1StrokeStyle>> strokes_;
    IDWriteTextRenderer* renderer_ = nullptr;
    std::vector<Com<ID2D1Layer>> layers_;
    ID2D1SolidColorBrush* brush(const Color& c);
    ID2D1StrokeStyle* stroke(const std::vector<float>& dash);
    ID2D1PathGeometry* path(const std::vector<Point>& pts, bool closed, bool filled);
};

inline D2D1_COLOR_F d2d(const Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
inline D2D1_RECT_F d2d(const Rect& r) { return D2D1::RectF(r.x, r.y, r.x + r.w, r.y + r.h); }
inline D2D1_POINT_2F d2d(Point p) { return D2D1::Point2F(p.x, p.y); }

/// The shared Direct2D factory.
ID2D1Factory* d2dFactory();

}  // namespace ui
