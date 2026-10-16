// The app's views: the shell (top bar, journey bar, editor, console, lesson
// strip) and one view per station, ported from the macOS version.
#pragma once

#include <set>

#include "AppModel.h"
#include "ListView.h"
#include "TextView.h"
#include "Widgets.h"

namespace ui {

using sp::CompilationP;

/// Every station's view can reload itself from the model.
class StationView : public View {
public:
    virtual void reload(AppModel& model, ModelEvent event) = 0;
};

/// Shown when the current source cannot reach this stage.
class EmptyNote : public View {
public:
    std::string title, message;
    void draw(Gfx& g, const Rect& dirty) override;
};

namespace Lessons {
std::pair<std::string, std::string> text(Station s);
}

// MARK: - Shell

/// The row of numbered stations from Source to Memory.
class JourneyBar : public View {
public:
    AppModel* model = nullptr;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseMoved(const MouseEvent& e) override;
    void mouseExited(const MouseEvent& e) override;
    void mouseDown(const MouseEvent& e) override;

private:
    std::vector<Rect> rects_;
    std::optional<int> hovered_;
    int status(Station s) const;
};

/// The explanation under the station view.
class LessonStrip : public View {
public:
    void setStation(Station s);
    void draw(Gfx& g, const Rect& dirty) override;

private:
    Station station_ = Station::Source;
};

/// Program output and problems, under the editor.
class ConsoleView : public View {
public:
    ConsoleView();
    Segmented tabs{{"Output", "Problems"}};
    ScrollView outputScroll;
    TextView outputView;
    ListPanel problems;
    AppModel* model = nullptr;
    std::function<void(int)> onSelectLine;

    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void reload(AppModel& m);

private:
    std::string lastText_;
    bool showingPlaceholder_ = false;
};

/// Title, example picker, language and optimisation level, and the clock controls.
class TopBar : public View {
public:
    TopBar();
    AppModel* model = nullptr;
    PopUp examples;
    Segmented lang{{"C", "Assembly"}};
    Segmented opt{{"-O0", "-O1"}};
    TButton reset{"", Icon::Reset};
    TButton back{"", Icon::StepBack};
    TButton stepCycle{"Cycle", Icon::StepCycle};
    TButton stepInst{"Instruction", Icon::StepInstruction};
    TButton stepLine{"Line", Icon::StepLine};
    TButton play{"Run", Icon::Play, TButton::Style::Primary};
    TButton toEnd{"", Icon::RunToEnd};
    Slider speed{6, 1};
    Label speedLabel{"", Fonts::tiny, Theme::secondary};
    static const double speeds[6];

    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void refresh();
};

/// Warns that a station shows the last program that compiled.
class StaleBanner : public View {
public:
    std::string text;
    void draw(Gfx& g, const Rect& dirty) override;
};

// MARK: - Editor

class CodeEditor;

/// The text view of the editor, with bands for pipeline stages, the selected line and errors.
class EditorTextView : public TextView {
public:
    CodeEditor* editor = nullptr;

protected:
    void drawBackground(Gfx& g, const Rect& dirty) override;
    void insertNewline() override;
};

/// Line numbers, error marks and pipeline stage badges, beside the text.
class LineRuler : public View {
public:
    CodeEditor* editor = nullptr;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseDown(const MouseEvent& e) override;
};

/// The one-line text field of the find bar.
class FindField : public TextView {
public:
    std::function<void(int)> onEnter;   // +1 = next, -1 = previous
    std::function<void()> onEscape;
    void keyDown(const KeyEvent& e) override;

protected:
    void insertNewline() override {}
};

/// A small find bar for the editor (Ctrl+F): a text field, next/previous and close.
class FindBar : public View {
public:
    FindBar();
    FindField field;
    TButton prev{"↑"};
    TButton next{"↓"};
    TButton close{"✕"};
    Label status{"", Fonts::small, Theme::secondary};
    std::function<void(int)> onFind;   // +1 next, -1 previous
    std::function<void()> onClose;
    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
};

/// The source editor: line numbers, syntax colours from the real lexer,
/// error underlines, and little stage badges showing which lines are in the pipeline right now.
class CodeEditor : public View {
public:
    CodeEditor();
    ScrollView scroll;
    EditorTextView textView;
    LineRuler ruler;
    FindBar findBar;
    std::function<void(const std::string&)> onChange;
    std::function<void(int)> onSelectLine;

    bool isAssembly() const { return isAssembly_; }
    void setErrors(std::vector<sp::Diagnostic> d);
    const std::vector<sp::Diagnostic>& errors() const { return errors_; }
    void setStages(const std::map<int, std::vector<int>>& s);
    const std::map<int, std::vector<int>>& stages() const { return stages_; }
    void setSelectedLine(std::optional<int> l);
    std::optional<int> selectedLine() const { return selectedLine_; }

    std::string text() const;
    void setText(const std::string& s, bool assembly);
    void highlight();
    std::optional<Rect> rectOfLine(int line) const;
    void reveal(int line);
    void select(sp::SourceRange r);
    void showFind();
    void find(int direction);

    void layout() override;
    bool editCommand(const std::string& cmd) override;

private:
    bool isAssembly_ = false;
    bool suppress_ = false;
    bool findVisible_ = false;
    std::vector<sp::Diagnostic> errors_;
    std::map<int, std::vector<int>> stages_;
    std::optional<int> selectedLine_;
    void refreshMarks();
};

// MARK: - Station views

class OverviewView : public StationView {
public:
    OverviewView();
    ListPanel panel;
    AppModel* model = nullptr;
    void layout() override { panel.setFrame(bounds()); }
    void reload(AppModel& m, ModelEvent e) override;
};

class TokenCanvas : public View {
public:
    struct Chip {
        Rect rect;
        sp::Token token;
        std::string label;
    };
    std::vector<Chip> chips;
    std::vector<std::pair<int, float>> lineLabels;
    float contentHeight = 100;
    std::optional<int> selectedLine;
    std::function<void(const sp::Token&)> onClick;
    std::vector<sp::Token> tokens;
    std::string summary;

    Size intrinsicSize() override { return {300, contentHeight}; }
    void layoutChips(float width);
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseMoved(const MouseEvent& e) override;
    void mouseExited(const MouseEvent& e) override;
    void mouseDown(const MouseEvent& e) override;

private:
    std::optional<int> hovered_;
};

class TokensView : public StationView {
public:
    TokensView();
    TokenCanvas canvas;
    CanvasPanel panel{&canvas};
    EmptyNote empty;
    AppModel* model = nullptr;
    void layout() override;
    void reload(AppModel& m, ModelEvent e) override;

private:
    float lastWidth_ = 0;
};

struct TreeNode {
    int depth = 0;
    std::string role, label, detail;
    int category = 0;   // 0 decl, 1 stmt, 2 expr, 3 type/struct
    int node = 0;
    sp::SourceRange range;
    int line = 0;
    bool hasChildren = false;
    bool isLast = false;
    std::vector<bool> ancestorsLast;
};

std::vector<TreeNode> buildTree(const sp::Program& p);

class TreeCanvas : public View {
public:
    std::vector<TreeNode> nodes;
    std::set<int> collapsed;
    std::optional<int> selectedLine;
    std::optional<int> selectedNode;
    std::function<void(const TreeNode&)> onClick;
    const float rowH = 23;
    const float indentW = 20;

    void recompute();
    Size intrinsicSize() override;
    float yOfVisible(int v) const { return 14 + v * rowH; }
    void reveal(std::optional<int> node, std::optional<int> line);
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseMoved(const MouseEvent& e) override;
    void mouseExited(const MouseEvent& e) override;
    void mouseDown(const MouseEvent& e) override;

private:
    std::vector<int> visible_;
    std::optional<int> hovered_;
    std::optional<int> visibleIndexAt(Point p) const;
};

class TreeView : public StationView {
public:
    TreeView();
    TreeCanvas canvas;
    CanvasPanel panel{&canvas};
    EmptyNote empty;
    TButton expand{"Expand all", Icon::ExpandAll};
    TButton collapseB{"Collapse expressions", Icon::CollapseAll};
    Label legend{"Declarations · Statements · Expressions (with their types from semantic analysis)", Fonts::small, Theme::secondary};
    AppModel* model = nullptr;
    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void reload(AppModel& m, ModelEvent e) override;
};

class SymbolsView : public StationView {
public:
    SymbolsView();
    ListPanel panel;
    EmptyNote empty;
    AppModel* model = nullptr;
    void layout() override;
    void reload(AppModel& m, ModelEvent e) override;
};

class PassList : public View {
public:
    struct Entry {
        std::string title;
        std::optional<sp::PassID> pass;
        std::optional<int> snapshot;
        int delta = 0;
        bool changed = false;
        bool enabled = true;
        bool isFinal = false;
    };
    std::vector<Entry> entries;
    int selected = 0;
    std::function<void(int)> onSelect;
    std::function<void(sp::PassID, bool)> onToggle;
    std::function<void(bool)> onPreset;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseDown(const MouseEvent& e) override;

private:
    std::vector<Rect> rects_, checkRects_, presetRects_;
};

class CFGCanvas : public View {
public:
    const sp::IRFunction* function = nullptr;
    sp::IRModuleP module;   // keeps the function alive
    std::set<int> changed;
    std::optional<int> selectedLine;
    const float boxW = 330;
    void recompute();
    Size intrinsicSize() override { return size_; }
    void draw(Gfx& g, const Rect& dirty) override;

private:
    std::map<int, Rect> boxes_;
    std::vector<int> order_;
    Size size_{400, 300};
};

class IRView : public StationView {
public:
    IRView();
    PassList passes;
    ListPanel listing;
    CFGCanvas cfg;
    CanvasPanel cfgPanel{&cfg};
    Segmented mode{{"Listing", "Control-flow graph"}};
    PopUp functionPicker{{"main"}};
    Label explanation{"", Fonts::small, Theme::secondary};
    EmptyNote empty;
    AppModel* model = nullptr;
    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void reload(AppModel& m, ModelEvent e) override;

private:
    std::vector<std::string> functions_;
};

class IntervalCanvas : public View {
public:
    const sp::Allocation* alloc = nullptr;
    const sp::IRFunction* function = nullptr;
    const float rowH = 17;
    const float labelW = 78;
    void recompute(float width);
    Size intrinsicSize() override { return size_; }
    void draw(Gfx& g, const Rect& dirty) override;

private:
    Size size_{300, 200};
};

class FrameCanvas : public View {
public:
    const sp::FrameLayout* frameLayout = nullptr;
    void recompute(float width);
    Size intrinsicSize() override { return size_; }
    void draw(Gfx& g, const Rect& dirty) override;

private:
    Size size_{300, 200};
    static float entryHeight(const sp::FrameLayout::Entry& e);
};

class AsmView : public StationView {
public:
    AsmView();
    ListPanel listing;
    IntervalCanvas intervals;
    CanvasPanel intervalPanel{&intervals};
    FrameCanvas frameCanvas;
    CanvasPanel framePanel{&frameCanvas};
    PopUp functionPicker{{"main"}};
    Segmented regs{{"All registers", "6", "3"}};
    EmptyNote empty;
    AppModel* model = nullptr;
    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void reload(AppModel& m, ModelEvent e) override;

private:
    std::vector<std::string> functions_;
    std::map<int, std::vector<uint32_t>> pcsByAsmLine_;
    CompilationP shownCompilation_;
};

class EncodingCanvas : public View {
public:
    std::optional<sp::LinkedInstruction> ins;
    std::function<std::optional<std::string>(uint32_t)> symbolize;
    void draw(Gfx& g, const Rect& dirty) override;
};

class MachineCodeView : public StationView {
public:
    MachineCodeView();
    EncodingCanvas encoding;
    ListPanel listing;
    Segmented scope{{"Your program", "Everything linked"}};
    EmptyNote empty;
    AppModel* model = nullptr;
    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void reload(AppModel& m, ModelEvent e) override;

private:
    std::optional<uint32_t> selectedPC_;
    void rebuild(AppModel& m);
};

class MemoryMapCanvas : public View {
public:
    sp::ExecutableP exe;
    std::optional<uint32_t> stackPointer;
    std::optional<uint32_t> brk;
    void draw(Gfx& g, const Rect& dirty) override;
};

class LinkView : public StationView {
public:
    LinkView();
    ListPanel listing;
    MemoryMapCanvas map;
    EmptyNote empty;
    AppModel* model = nullptr;
    void layout() override;
    void reload(AppModel& m, ModelEvent e) override;
};

class DatapathCanvas : public View {
public:
    AppModel* model = nullptr;
    void draw(Gfx& g, const Rect& dirty) override;
};

class PipelineChart : public View {
public:
    AppModel* model = nullptr;
    std::function<void(uint32_t)> onClickPC;
    const float rowH = 21;
    const float colW = 34;
    const float labelW = 210;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseDown(const MouseEvent& e) override;

private:
    std::vector<uint32_t> rowPCs_;
};

class CPUStatsCanvas : public View {
public:
    AppModel* model = nullptr;
    void draw(Gfx& g, const Rect& dirty) override;
};

class PipelineView : public StationView {
public:
    PipelineView();
    DatapathCanvas datapath;
    PipelineChart chart;
    CPUStatsCanvas stats;
    Segmented forwarding{{"Forwarding on", "off"}};
    Segmented branchStage{{"ID", "EX", "MEM"}};
    PopUp predictor;
    Label branchLabel{"Branches resolved in", Fonts::small, Theme::secondary};
    Check caches{"Caches", true};
    EmptyNote empty;
    AppModel* model = nullptr;
    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void reload(AppModel& m, ModelEvent e) override;
};

class HierarchyCanvas : public View {
public:
    AppModel* model = nullptr;
    int selectedCache = 1;
    std::function<void(int)> onSelectCache;
    void draw(Gfx& g, const Rect& dirty) override;
    void mouseDown(const MouseEvent& e) override;

private:
    std::map<int, Rect> cacheRects_;
};

class CacheGridCanvas : public View {
public:
    const sp::Cache* cache = nullptr;
    int now = 0;
    const float rowH = 20;
    void recompute(float width);
    Size intrinsicSize() override { return size_; }
    void draw(Gfx& g, const Rect& dirty) override;

private:
    Size size_{300, 300};
};

class CacheStatsCanvas : public View {
public:
    const sp::Cache* cache = nullptr;
    std::vector<sp::MemoryAccess> recent;
    void draw(Gfx& g, const Rect& dirty) override;
};

class MemoryView : public StationView {
public:
    MemoryView();
    HierarchyCanvas hierarchy;
    CacheGridCanvas grid;
    CanvasPanel gridPanel{&grid};
    CacheStatsCanvas stats;
    Segmented which{{"L1 I-cache", "L1 D-cache", "L2"}};
    PopUp sizeP{{}}, blockP{{}}, waysP{{}}, replP{{}}, writeP{{}};
    Check enabled{"Caches on", true};
    Check l2On{"L2", true};
    EmptyNote empty;
    AppModel* model = nullptr;
    void layout() override;
    void draw(Gfx& g, const Rect& dirty) override;
    void reload(AppModel& m, ModelEvent e) override;

private:
    int selected_ = 1;
    const std::vector<int> sizes_{64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768};
    const std::vector<int> blocks_{4, 8, 16, 32, 64, 128};
    const std::vector<int> waysList_{1, 2, 4, 8, 16};
    void edit(const std::function<void(sp::CacheConfig&)>& f);
};

// MARK: - Main view

class MainView : public View {
public:
    explicit MainView(AppModel& model);
    AppModel& model;
    TopBar top;
    JourneyBar journey;
    SplitView split{true};
    SplitView leftSplit{false};
    View editorCard;
    CodeEditor editor;
    ConsoleView console;
    View right;
    View stationHost;
    LessonStrip lesson;
    Label editorTitle{"", Fonts::subheading, Theme::heading};
    StaleBanner stale;

    OverviewView overview;
    TokensView tokens;
    TreeView tree;
    SymbolsView symbols;
    IRView ir;
    AsmView asmView;
    MachineCodeView machine;
    LinkView link;
    PipelineView pipeline;
    MemoryView memory;
    std::map<Station, StationView*> views;

    void layout() override;
    void start();

private:
    bool didInitialSplit_ = false;
    std::set<Station> dirtyOthers_;
    void updateStale();
    void showStation();
    void handle(ModelEvent e);
};

}  // namespace ui
