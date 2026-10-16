// Points, sizes, rectangles and colours in device-independent pixels (DIPs).
// Every view uses a top-left origin with y growing downwards.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ui {

struct Point {
    float x = 0, y = 0;
    Point() = default;
    Point(float x_, float y_) : x(x_), y(y_) {}
    Point operator+(Point o) const { return {x + o.x, y + o.y}; }
    Point operator-(Point o) const { return {x - o.x, y - o.y}; }
    bool operator==(const Point&) const = default;
};

struct Size {
    float w = 0, h = 0;
    Size() = default;
    Size(float w_, float h_) : w(w_), h(h_) {}
    bool operator==(const Size&) const = default;
};

struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
    Rect() = default;
    Rect(float x_, float y_, float w_, float h_) : x(x_), y(y_), w(w_), h(h_) {}
    Rect(Point o, Size s) : x(o.x), y(o.y), w(s.w), h(s.h) {}

    float minX() const { return x; }
    float minY() const { return y; }
    float maxX() const { return x + w; }
    float maxY() const { return y + h; }
    float midX() const { return x + w / 2; }
    float midY() const { return y + h / 2; }
    Point origin() const { return {x, y}; }
    Size size() const { return {w, h}; }
    bool isEmpty() const { return w <= 0 || h <= 0; }
    Rect insetBy(float dx, float dy) const { return {x + dx, y + dy, w - 2 * dx, h - 2 * dy}; }
    Rect offsetBy(float dx, float dy) const { return {x + dx, y + dy, w, h}; }
    bool contains(Point p) const { return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h; }
    bool intersects(const Rect& o) const { return x < o.maxX() && o.x < maxX() && y < o.maxY() && o.y < maxY(); }
    Rect intersection(const Rect& o) const {
        float a = std::max(x, o.x), b = std::max(y, o.y);
        float c = std::min(maxX(), o.maxX()), d = std::min(maxY(), o.maxY());
        if (c <= a || d <= b) return {a, b, 0, 0};
        return {a, b, c - a, d - b};
    }
    bool operator==(const Rect&) const = default;
};

struct Color {
    float r = 0, g = 0, b = 0, a = 0;
    Color() = default;
    Color(float r_, float g_, float b_, float a_ = 1) : r(r_), g(g_), b(b_), a(a_) {}
    static Color hex(uint32_t v, float alpha = 1) {
        return {((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, alpha};
    }
    static Color white() { return {1, 1, 1, 1}; }
    static Color black() { return {0, 0, 0, 1}; }
    static Color clear() { return {0, 0, 0, 0}; }
    Color alpha(float a_) const { return {r, g, b, a_}; }
    /// Like NSColor.blended(withFraction:of:).
    Color blended(float f, Color o) const { return {r + (o.r - r) * f, g + (o.g - g) * f, b + (o.b - b) * f, a + (o.a - a) * f}; }
    bool isClear() const { return a <= 0; }
    uint32_t key() const {
        auto c = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255); };
        return c(r) << 24 | c(g) << 16 | c(b) << 8 | c(a);
    }
    bool operator==(const Color& o) const { return key() == o.key(); }
};

}  // namespace ui
