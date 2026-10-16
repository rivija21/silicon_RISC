// A fast, custom-drawn list of rows (used for IR, assembly, machine code, tables…),
// and scroll-view wrappers for lists and canvases.
#pragma once

#include <functional>
#include <optional>

#include "Theme.h"
#include "View.h"

namespace ui {

/// One cell of a listing row.
struct Cell {
    AttrString text;
    /// nullopt = take the remaining width.
    std::optional<float> width;
    Align align = Align::Left;
    /// A rounded background behind the text (a badge).
    std::optional<Color> badge;
    float leftPad = 0;

    Cell(AttrString t, std::optional<float> w = std::nullopt, Align a = Align::Left, std::optional<Color> b = std::nullopt, float pad = 0)
        : text(std::move(t)), width(w), align(a), badge(b), leftPad(pad) {}
    Cell(const std::string& s, const Font& font = Fonts::code, const Color& color = Theme::text, std::optional<float> w = std::nullopt,
         Align a = Align::Left)
        : text(s, font, color), width(w), align(a) {}
};

struct Row {
    enum class Kind { Normal, Header, Section, Spacer };
    std::vector<Cell> cells;
    Kind kind = Kind::Normal;
    float height = 19;
    std::optional<Color> background;
    /// Payload for click handling and highlight matching.
    int tag = -1;
    std::optional<int> line;
    std::optional<uint32_t> pc;
    std::optional<int> node;
    std::optional<int> irInst;
    /// A coloured strip at the left edge (stage colour, etc.).
    std::optional<Color> marker;
    float indent = 0;
    std::string tooltip;

    Row() = default;
    explicit Row(std::vector<Cell> c, Kind k = Kind::Normal, float h = 19, int t = -1) : cells(std::move(c)), kind(k), height(h), tag(t) {}
    static Row header(const std::string& s, const std::string& detail = "");
    static Row spacer(float h = 8) { return Row({}, Kind::Spacer, h); }
};

class ListView : public View {
public:
    void setRows(std::vector<Row> rows);
    const std::vector<Row>& rows() const { return rows_; }
    std::vector<Row>& mutableRows() { return rows_; }
    float contentHeight() const { return contentHeight_; }
    float padding = 10;
    /// Returns a highlight colour for a row (selection, execution…).
    std::function<std::optional<Color>(const Row&)> highlight;
    std::function<void(int, const Row&)> onClick;
    float minWidth = 0;

    std::optional<int> indexAt(float y) const;
    Rect rectOfRow(int k) const;
    /// Scrolls so that row k is visible (centred if it was far away).
    void reveal(int k);

    void draw(Gfx& g, const Rect& dirty) override;
    void mouseMoved(const MouseEvent& e) override;
    void mouseExited(const MouseEvent& e) override;
    void mouseDown(const MouseEvent& e) override;
    std::string tooltipAt(Point p) override;

private:
    std::vector<Row> rows_;
    std::vector<float> offsets_;
    float contentHeight_ = 0;
    std::optional<int> hovered_;
    void drawRow(Gfx& g, const Row& r, int k, float y);
    void setHovered(std::optional<int> h);
};

/// A ListView inside a scroll view, sized to fill.
class ListPanel : public View {
public:
    ListPanel();
    ScrollView scroll;
    ListView list;
    void layout() override;
    void set(std::vector<Row> rows);
};

/// A scrollable canvas for custom diagrams. The canvas's intrinsic size is its preferred size.
class CanvasPanel : public View {
public:
    explicit CanvasPanel(View* canvas);
    ScrollView scroll;
    View* canvas;
    void layout() override;
};

}  // namespace ui
