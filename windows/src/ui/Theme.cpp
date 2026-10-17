#include "Theme.h"

#include <cmath>
#include <map>

#include "../core/SiliconCore.h"

namespace ui {

Color Theme::fieldColor(const std::string& name) {
    static const std::map<std::string, Color> field = {
        {"opcode", Color::hex(0xE8D5F0)}, {"rd", Color::hex(0xD3ECDF)},     {"rs1", Color::hex(0xD6E6F7)},   {"rs2", Color::hex(0xDCE3F9)},
        {"funct3", Color::hex(0xF7E3C7)}, {"funct7", Color::hex(0xF5D9D2)}, {"imm", Color::hex(0xFFF1B8)}, {"shamt", Color::hex(0xFFF1B8)},
    };
    if (name.rfind("imm", 0) == 0) return field.at("imm");
    auto it = field.find(name);
    return it == field.end() ? Color::hex(0xEEEEEE) : it->second;
}

static Gfx* current = nullptr;
Gfx& G() { return *current; }
void setCurrentGfx(Gfx* g) { current = g; }

void drawText(const std::string& s, Point p, const Font& f, const Color& c) { G().drawText(s, p, f, c); }
void drawText(const std::string& s, const Rect& r, const Font& f, const Color& c, Align align) { G().drawText(s, r, f, c, align); }

void drawAttributed(const AttrString& a, const Rect& r) {
    TextBox box;
    box.wrap = true;
    box.truncate = true;
    G().drawAttributed(a, r, box);
}

void fillRounded(const Rect& r, float radius, const Color& c) { G().fillRounded(r, radius, c); }
void strokeRounded(const Rect& r, float radius, const Color& c, float width) { G().strokeRounded(r, radius, c, width); }
void fillRect(const Rect& r, const Color& c) { G().fillRect(r, c); }
void strokeRect(const Rect& r, const Color& c, float width) { G().strokeRect(r, c, width); }
void line(Point a, Point b, const Color& c, float width, const std::vector<float>& dash) { G().line(a, b, c, width, dash); }

void arrowHead(Point tip, Point from, const Color& c, float size) {
    float ang = std::atan2(tip.y - from.y, tip.x - from.x);
    G().fillPolygon({tip, Point(tip.x - size * std::cos(ang - 0.45f), tip.y - size * std::sin(ang - 0.45f)),
                     Point(tip.x - size * std::cos(ang + 0.45f), tip.y - size * std::sin(ang + 0.45f))},
                    c);
}

void drawCard(const Rect& r, float radius, const Color& fill) {
    // A soft shadow just below the card.
    G().fillRounded(r.offsetBy(0, 1).insetBy(-0.5f, -0.5f), radius + 0.5f, Color::hex(0x6B5A3A, 0.06f));
    G().fillRounded(r.offsetBy(0, 1.5f), radius, Color::hex(0x6B5A3A, 0.05f));
    fillRounded(r, radius, fill);
    strokeRounded(r, radius, Theme::border);
}

Rect drawPill(const std::string& s, Point p, const Font& font, const Color& fill, const Color& text) {
    Size sz = textSize(s, font);
    Rect r{p.x, p.y, sz.w + 10, sz.h + 3};
    fillRounded(r, r.h / 2, fill);
    drawText(s, Point(r.minX() + 5, r.minY() + 1.5f), font, text);
    return r;
}

std::string formatCount(long long n) {
    std::string digits = std::to_string(n < 0 ? -n : n);
    std::string out;
    int k = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (k > 0 && k % 3 == 0) out += ',';
        out += *it;
        k++;
    }
    if (n < 0) out += '-';
    return std::string(out.rbegin(), out.rend());
}

std::string hex8(uint32_t v) { return sp::format("%08x", v); }
std::string hexAddr(uint32_t v) { return sp::format("0x%08x", v); }

std::string binaryString(uint32_t v, int bits) {
    std::string s;
    for (int k = bits - 1; k >= 0; k--) s += ((v >> k) & 1) ? '1' : '0';
    return s;
}

AttrString highlightAsm(const std::string& text, const Font& font, bool dim) {
    AttrString out;
    std::string body = text, comment;
    size_t h = text.find('#');
    if (h != std::string::npos) {
        body = text.substr(0, h);
        comment = text.substr(h);
    }
    std::string trimmed = sp::trimmed(body);
    size_t leadLen = 0;
    while (leadLen < body.size() && (body[leadLen] == ' ' || body[leadLen] == '\t')) leadLen++;
    out.add(body.substr(0, leadLen), font, Theme::text);
    Font bold = Fonts::mono(font.size, Weight::Semibold);
    auto splitFirst = [](const std::string& s) {
        size_t k = s.find(' ');
        if (k == std::string::npos) return std::vector<std::string>{s};
        return std::vector<std::string>{s.substr(0, k), s.substr(k + 1)};
    };
    if (sp::hasSuffix(trimmed, ":")) {
        out.add(trimmed, bold, Theme::label);
    } else if (sp::hasPrefix(trimmed, ".")) {
        auto parts = splitFirst(trimmed);
        out.add(parts[0], font, Theme::directive);
        if (parts.size() > 1) out.add(" " + parts[1], font, Theme::string);
    } else if (!trimmed.empty()) {
        auto parts = splitFirst(trimmed);
        out.add(parts[0], bold, dim ? Theme::secondary : Theme::mnemonic);
        if (parts.size() > 1) {
            out.add(" ", font, Theme::text);
            std::string tok;
            auto flush = [&] {
                if (tok.empty()) return;
                Color color;
                if (sp::Reg::parse(tok)) color = Theme::reg;
                else if ((tok[0] >= '0' && tok[0] <= '9') || tok[0] == '-') color = Theme::number;
                else if (tok[0] == '%') color = Theme::purple;
                else color = Theme::label;
                out.add(tok, font, color);
                tok.clear();
            };
            for (char c : parts[1]) {
                if (c == ',' || c == '(' || c == ')' || c == ' ') {
                    flush();
                    out.add(std::string(1, c), font, Theme::punct);
                } else {
                    tok += c;
                }
            }
            flush();
        }
    }
    // Keep the spaces before a comment, so that run offsets match the original text.
    if (leadLen + trimmed.size() < body.size()) out.add(body.substr(leadLen + trimmed.size()), font, Theme::text);
    if (!comment.empty()) out.add(comment, Fonts::mono(font.size - 0.5f), Theme::comment);
    return out;
}

}  // namespace ui
