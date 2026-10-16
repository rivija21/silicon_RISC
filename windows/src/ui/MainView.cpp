#include "Views.h"

namespace ui {

using namespace sp;

MainView::MainView(AppModel& m) : model(m) {
    backgroundColor = Theme::background;
    top.model = &model;
    journey.model = &model;
    console.model = &model;
    addSubview(&top);
    addSubview(&journey);

    split.minPosition = [] { return 330.0f; };
    split.maxPosition = [this] { return split.width() - 620; };
    split.resizingPane = 1;
    split.dividerColor = Color::clear();
    leftSplit.minPosition = [] { return 160.0f; };
    leftSplit.maxPosition = [this] { return leftSplit.height() - 90; };
    leftSplit.resizingPane = 0;
    leftSplit.dividerColor = Color::clear();
    split.onResize = [this] { setNeedsLayout(); };
    leftSplit.onResize = [this] { setNeedsLayout(); };

    editorCard.backgroundColor = Theme::panel;
    editorCard.cornerRadius = 8;
    editorCard.borderColor = Theme::border;
    editorCard.borderWidth = 1;
    editorCard.masksToBounds = true;
    editorCard.addSubview(&editor);
    editorCard.addSubview(&editorTitle);
    leftSplit.setPanes(&editorCard, &console);
    split.setPanes(&leftSplit, &right);
    addSubview(&split);

    stationHost.backgroundColor = Theme::panel;
    stationHost.cornerRadius = 8;
    stationHost.borderColor = Theme::border;
    stationHost.borderWidth = 1;
    stationHost.masksToBounds = true;
    right.addSubview(&stationHost);
    right.addSubview(&lesson);
    right.addSubview(&stale);
    stale.setHidden(true);

    views = {{Station::Source, &overview}, {Station::Tokens, &tokens},  {Station::Tree, &tree},       {Station::Symbols, &symbols},
             {Station::IR, &ir},           {Station::Asm, &asmView},    {Station::Machine, &machine}, {Station::Link, &link},
             {Station::Pipeline, &pipeline}, {Station::Memory, &memory}};
    for (auto& [s, v] : views) {
        v->setHidden(true);
        stationHost.addSubview(v);
    }

    editor.onChange = [this](const std::string& s) { model.setSource(s); };
    editor.onSelectLine = [this](int l) { model.selectLine(l, Station::Source); };
    console.onSelectLine = [this](int l) {
        model.selectLine(l, Station::Source);
        editor.reveal(l);
    };
    model.observe([this](ModelEvent e) { handle(e); });
    editor.setText(model.source(), model.isAssembly());
}

void MainView::layout() {
    top.setFrame({0, 0, width(), 50});
    journey.setFrame({0, 52, width(), 70});
    split.setFrame({10, 124, width() - 20, height() - 134});
    if (!didInitialSplit_ && split.width() > 0) {
        didInitialSplit_ = true;
        split.setPosition(std::max(420.0f, split.width() * 0.36f));
        split.layoutSubtreeIfNeeded();
        leftSplit.setPosition(leftSplit.height() - 190);
        split.layoutSubtreeIfNeeded();
    }
    editorTitle.setFrame({12, 7, editorCard.width() - 24, 18});
    editor.setFrame({0, 30, editorCard.width(), editorCard.height() - 30});
    float lh = 96;
    float bannerH = stale.hidden() ? 0 : 34;
    stale.setFrame({8, 0, right.width() - 8, 28});
    stationHost.setFrame({8, bannerH, right.width() - 8, right.height() - lh - 8 - bannerH});
    lesson.setFrame({8, right.height() - lh, right.width() - 8, lh});
    for (auto& [s, v] : views) v->setFrame(stationHost.bounds());
}

void MainView::start() {
    model.compileNow();
    showStation();
}

/// Warns when a station shows the last program that compiled, not the text in the editor.
void MainView::updateStale() {
    bool show = false;
    CompilationP c = model.compilation();
    if (c && c->failedStage && model.good()) {
        static const std::vector<Stage> stageOrder = {Stage::Lexer, Stage::Preprocessor, Stage::Parser, Stage::Semantic,
                                                      Stage::IR, Stage::Codegen, Stage::Assembler, Stage::Linker};
        static const std::map<Station, Stage> need = {{Station::Tokens, Stage::Lexer},     {Station::Tree, Stage::Parser},
                                                      {Station::Symbols, Stage::Semantic}, {Station::IR, Stage::IR},
                                                      {Station::Asm, Stage::Codegen},      {Station::Machine, Stage::Assembler},
                                                      {Station::Link, Stage::Linker},      {Station::Pipeline, Stage::Linker},
                                                      {Station::Memory, Stage::Linker}};
        Stage failed = *c->failedStage;
        auto st = need.find(model.station());
        auto a = st == need.end() ? stageOrder.end() : std::find(stageOrder.begin(), stageOrder.end(), st->second);
        auto b = std::find(stageOrder.begin(), stageOrder.end(), failed);
        if (a != stageOrder.end() && b != stageOrder.end() && a >= b) show = !(model.station() == Station::Tokens && failed != Stage::Lexer);
        auto errs = c->errors();
        int line = errs.empty() ? 0 : errs[0].line;
        stale.text = cat("Showing the last version that compiled. ", stageName(failed), " stopped at an error",
                         line > 0 ? cat(" on line ", line) : std::string(""), " — fix it to update this view.");
    }
    if (stale.hidden() == show) {
        stale.setHidden(!show);
        setNeedsLayout();
    }
    stale.setNeedsDisplay();
}

void MainView::showStation() {
    updateStale();
    for (auto& [s, v] : views) v->setHidden(s != model.station());
    lesson.setStation(model.station());
    journey.setNeedsDisplay();
    views[model.station()]->reload(model, ModelEvent::Compiled);
}

void MainView::handle(ModelEvent e) {
    switch (e) {
    case ModelEvent::Compiled: {
        if (editor.text() != model.source() || editor.isAssembly() != model.isAssembly()) editor.setText(model.source(), model.isAssembly());
        editor.setErrors(model.compilation() ? model.compilation()->diagnostics : std::vector<Diagnostic>{});
        editorTitle.setText(model.isAssembly() ? "Assembly source" : "C source");
        views[model.station()]->reload(model, ModelEvent::Compiled);
        for (auto& [s, v] : views)
            if (s != model.station() && !v->hidden()) v->reload(model, ModelEvent::Compiled);
        dirtyOthers_.clear();
        for (int k = 0; k < sp::stationCount; k++)
            if ((Station)k != model.station()) dirtyOthers_.insert((Station)k);
        editor.setStages(model.stagesByLine());
        console.reload(model);
        top.refresh();
        journey.setNeedsDisplay();
        updateStale();
        break;
    }
    case ModelEvent::Stepped: case ModelEvent::Running: case ModelEvent::Config:
        editor.setStages(model.stagesByLine());
        console.reload(model);
        top.refresh();
        views[model.station()]->reload(model, e);
        if (e == ModelEvent::Config) {
            dirtyOthers_.clear();
            for (int k = 0; k < sp::stationCount; k++)
                if ((Station)k != model.station()) dirtyOthers_.insert((Station)k);
        }
        journey.setNeedsDisplay();
        break;
    case ModelEvent::Selection:
        editor.setSelectedLine(model.selection().line);
        if (model.selection().origin != Station::Source) {
            if (model.selection().range) editor.select(*model.selection().range);
            if (model.selection().line) editor.reveal(*model.selection().line);
        }
        views[model.station()]->reload(model, ModelEvent::Selection);
        break;
    case ModelEvent::Station:
        if (dirtyOthers_.count(model.station())) {
            dirtyOthers_.erase(model.station());
            views[model.station()]->reload(model, ModelEvent::Compiled);
        }
        showStation();
        views[model.station()]->reload(model, ModelEvent::Stepped);
        views[model.station()]->reload(model, ModelEvent::Selection);
        break;
    }
}

}  // namespace ui
