// The app's look (a classic lab notebook: warm parchment, ivory cards,
// ink-blue and crimson), its fonts, and the drawing helpers every view uses.
#pragma once

#include <string>

#include "Gfx.h"

namespace ui {

namespace Theme {
inline const Color background = Color::hex(0xF3EDE2);
inline const Color panel = Color::hex(0xFFFDF8);
inline const Color panelAlt = Color::hex(0xFAF5EA);
inline const Color border = Color::hex(0xE3D9C6);
inline const Color borderStrong = Color::hex(0xCDBFA5);
inline const Color grid = Color::hex(0xEDF1EA);
inline const Color gridMajor = Color::hex(0xD5DFD2);
inline const Color text = Color::hex(0x2B2A33);
inline const Color secondary = Color::hex(0x7C7364);
inline const Color faint = Color::hex(0xA79F92);
inline const Color ink = Color::hex(0x1F4E9E);
inline const Color crimson = Color::hex(0xC0392B);
inline const Color heading = Color::hex(0xA0472E);
inline const Color green = Color::hex(0x1C7C6A);
inline const Color amber = Color::hex(0xC27A1A);
inline const Color purple = Color::hex(0x7B3F9E);
inline const Color selection = Color::hex(0xFCE9B8);
inline const Color selectionStrong = Color::hex(0xF6D57A);
inline const Color hover = Color::hex(0xF4EEDF);
inline const Color errorFill = Color::hex(0xF9DEDA);
inline const Color okFill = Color::hex(0xDCEFE5);

// Pipeline stages.
inline const Color stage[5] = {Color::hex(0x2F6FC9), Color::hex(0x2A8F62), Color::hex(0xD27A22), Color::hex(0x8552B8), Color::hex(0xBF3D5A)};
inline Color stageFill(int k) { return stage[k].blended(0.82f, Color::white()); }

// Syntax colours.
inline const Color keyword = Color::hex(0x8E3A9E);
inline const Color number = Color::hex(0x1F6FB2);
inline const Color string = Color::hex(0xB5552B);
inline const Color comment = Color::hex(0x6E8B5E);
inline const Color directive = Color::hex(0xA0472E);
inline const Color punct = Color::hex(0x6A6358);
inline const Color type = Color::hex(0x1C7C6A);
inline const Color reg = Color::hex(0x1C7C6A);
inline const Color label = Color::hex(0xA0472E);
inline const Color mnemonic = Color::hex(0x1F4E9E);

/// Background of an instruction-encoding field.
Color fieldColor(const std::string& name);
}  // namespace Theme

namespace Fonts {
inline Font serif(float size, Weight w = Weight::Regular) { return Font{Family::Serif, size, w, false}; }
inline Font mono(float size = 12, Weight w = Weight::Regular) { return Font{Family::Mono, size, w, false}; }
inline Font ui(float size = 12, Weight w = Weight::Regular) { return Font{Family::UI, size, w, false}; }
inline const Font title = serif(18, Weight::Semibold);
inline const Font heading = serif(14, Weight::Semibold);
inline const Font subheading = serif(12.5f, Weight::Semibold);
inline const Font body = ui(12);
inline const Font small = ui(11);
inline const Font tiny = ui(10);
inline const Font code = mono(12.5f);
inline const Font codeSmall = mono(11);
inline const Font codeTiny = mono(10);
}  // namespace Fonts

/// The surface being drawn on right now (set by the window while it paints).
Gfx& G();
void setCurrentGfx(Gfx* g);

// MARK: - Drawing helpers (the same names as in the original views)

void drawText(const std::string& s, Point p, const Font& f, const Color& c = Theme::text);
/// Draws text so that it fits the rect, centred vertically, truncated with an ellipsis.
void drawText(const std::string& s, const Rect& r, const Font& f, const Color& c = Theme::text, Align align = Align::Left);
void drawAttributed(const AttrString& a, const Rect& r);
void fillRounded(const Rect& r, float radius, const Color& c);
void strokeRounded(const Rect& r, float radius, const Color& c, float width = 1);
void fillRect(const Rect& r, const Color& c);
void strokeRect(const Rect& r, const Color& c, float width = 1);
void line(Point a, Point b, const Color& c, float width = 1, const std::vector<float>& dash = {});
void arrowHead(Point tip, Point from, const Color& c, float size = 6);
/// A softly shadowed ivory card, the basic panel of the app.
void drawCard(const Rect& r, float radius = 8, const Color& fill = Theme::panel);
/// A small rounded label like "load-use" or "L1 hit". Returns its rect.
Rect drawPill(const std::string& s, Point p, const Font& font, const Color& fill, const Color& text = Theme::text);

std::string formatCount(long long n);
std::string hex8(uint32_t v);
std::string hexAddr(uint32_t v);
std::string binaryString(uint32_t v, int bits);
/// Colours an assembly line: mnemonic, registers, numbers, labels, comments.
AttrString highlightAsm(const std::string& text, const Font& font = Fonts::code, bool dim = false);

}  // namespace ui
