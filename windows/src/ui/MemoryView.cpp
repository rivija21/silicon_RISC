#include "Views.h"

namespace ui {

using namespace sp;

static std::string sizeText(int b) { return b >= 1024 ? cat(b / 1024, " KiB") : cat(b, " B"); }

// MARK: - Hierarchy

void HierarchyCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panelAlt);
    line({width() - 0.5f, 0}, {width() - 0.5f, height()}, Theme::border);
    Pipeline* p = model ? model->pipeline() : nullptr;
    if (!p) return;
    const MemoryHierarchy& h = p->hierarchy;
    drawText("Memory hierarchy", Point(12, 8), Fonts::subheading, Theme::heading);
    drawText("small & fast at the top, big & slow at the bottom", Point(12, 28), Fonts::tiny, Theme::secondary);
    float W = width() - 24;
    float y = 50;
    cacheRects_.clear();
    std::optional<MemoryAccess> lastFetch, lastData;
    for (auto it = h.recent.rbegin(); it != h.recent.rend(); ++it) {
        if (!lastFetch && it->kind == AccessKind::Fetch) lastFetch = *it;
        if (!lastData && it->kind != AccessKind::Fetch) lastData = *it;
        if (lastFetch && lastData) break;
    }
    auto level = [&](const Rect& r, const std::string& title, const std::string& detail, const CacheStats* stats, const Color& fill, bool lit,
                     std::optional<int> selectable = std::nullopt) {
        drawCard(r, 7, fill);
        if (selectable && *selectable == selectedCache) strokeRounded(r, 7, Theme::ink, 2);
        if (lit) strokeRounded(r.insetBy(-2, -2), 9, Theme::amber, 2);
        drawText(title, Point(r.minX() + 8, r.minY() + 4), Fonts::ui(11.5f, Weight::Bold), Theme::text);
        drawText(detail, Rect(r.minX() + 8, r.minY() + 21, r.w - 14, 13), Fonts::tiny, Theme::secondary);
        if (stats) {
            const CacheStats& s = *stats;
            std::string rate = s.accesses == 0 ? "—" : format("%.1f%% hits", 100 * s.hitRate());
            Color rc = s.accesses == 0 ? Theme::faint : s.hitRate() > 0.9 ? Theme::green : s.hitRate() > 0.7 ? Theme::amber : Theme::crimson;
            drawText(rate, Rect(r.minX() + 8, r.minY() + 35, r.w - 14, 15), Fonts::mono(11, Weight::Bold), rc);
            drawText(cat(formatCount(s.hits), " / ", formatCount(s.accesses)), Rect(r.minX() + 8, r.minY() + 51, r.w - 14, 13), Fonts::mono(9.5f), Theme::secondary);
            Rect bar{r.minX() + 8, r.maxY() - 9, r.w - 16, 4};
            fillRounded(bar, 2, Theme::errorFill);
            fillRounded({bar.minX(), bar.minY(), bar.w * (float)s.hitRate(), 4}, 2, Theme::green);
        }
        if (selectable) cacheRects_[*selectable] = r;
    };
    Rect regR{12 + W * 0.2f, y, W * 0.6f, 40};
    level(regR, "Registers", "32 × 4 bytes", nullptr, Color::hex(0xE5F1EA), false);
    y = regR.maxY() + 22;
    const HierarchyConfig& cfg = h.config;
    float half = (W - 10) / 2;
    Rect iR{12, y, half, 74};
    Rect dR{12 + half + 10, y, half, 74};
    bool disabled = !cfg.enabled;
    int cycle = p->cycle();
    level(iR, "L1 I-cache", disabled ? "off (perfect memory)" : cat(sizeText(cfg.l1i.size), ", ", cfg.l1i.associativityName()),
          disabled ? nullptr : &h.l1i.stats(), Theme::panel, lastFetch && lastFetch->time == cycle && lastFetch->servedBy != "L1", 0);
    level(dR, "L1 D-cache", disabled ? "off (perfect memory)" : cat(sizeText(cfg.l1d.size), ", ", cfg.l1d.associativityName()),
          disabled ? nullptr : &h.l1d.stats(), Theme::panel, lastData && lastData->time == cycle && lastData->servedBy != "L1", 1);
    line({regR.midX() - 30, regR.maxY()}, {iR.midX(), iR.minY()}, Theme::borderStrong, 1.5f);
    line({regR.midX() + 30, regR.maxY()}, {dR.midX(), dR.minY()}, Theme::borderStrong, 1.5f);
    drawText("1 cycle", Point(dR.maxX() - 40, dR.minY() - 15), Fonts::tiny, Theme::secondary);
    y = iR.maxY() + 24;
    if (h.l2) {
        const Cache& l2 = *h.l2;
        Rect r{12 + W * 0.1f, y, W * 0.8f, 74};
        bool lit = (lastData && lastData->time == cycle && lastData->servedBy == "DRAM") || (lastFetch && lastFetch->time == cycle && lastFetch->servedBy == "DRAM");
        level(r, "L2 cache (unified)", cat(sizeText(l2.config.size), ", ", l2.config.associativityName()), &l2.stats(), Theme::panel, lit, 2);
        line({iR.midX(), iR.maxY()}, {r.midX() - 40, r.minY()}, Theme::borderStrong, 1.5f);
        line({dR.midX(), dR.maxY()}, {r.midX() + 40, r.minY()}, Theme::borderStrong, 1.5f);
        drawText(cat("+", l2.config.latency, " cycles"), Point(r.maxX() - 62, r.minY() - 15), Fonts::tiny, Theme::secondary);
        y = r.maxY() + 24;
    }
    Rect dram{12, y, W, 48};
    drawCard(dram, 7, Color::hex(0xF1E9DA));
    drawText("Main memory (DRAM)", Point(dram.minX() + 8, dram.minY() + 5), Fonts::ui(11.5f, Weight::Bold), Theme::text);
    drawText(disabled ? "every access takes 1 cycle (ideal)" : cat("+", cfg.dramLatency, " cycles · ", formatCount(h.dramAccesses), " accesses"),
             Rect(dram.minX() + 8, dram.minY() + 24, dram.w - 16, 14), Fonts::small, Theme::secondary);
    if (h.l2) {
        line({dram.midX(), dram.minY() - 24}, {dram.midX(), dram.minY()}, Theme::borderStrong, 1.5f);
    } else {
        line({iR.midX(), iR.maxY()}, {dram.midX() - 30, dram.minY()}, Theme::borderStrong, 1.5f);
        line({dR.midX(), dR.maxY()}, {dram.midX() + 30, dram.minY()}, Theme::borderStrong, 1.5f);
    }
    y = dram.maxY() + 16;
    // AMAT.
    if (!disabled) {
        double amat = h.amat(h.l1d);
        std::string l2part = h.l2 ? cat(h.l2->config.latency, " + ", format("%.2f", h.l2->stats().missRate()), " × ", cfg.dramLatency)
                                  : std::to_string(cfg.dramLatency);
        drawText("Average memory access time (data)", Point(12, y), Fonts::ui(11, Weight::Semibold), Theme::heading);
        y += 16;
        drawText(cat("AMAT = 1 + ", format("%.3f", h.l1d.stats().missRate()), " × (", l2part, ")"), Rect(12, y, W, 14), Fonts::mono(10), Theme::text);
        y += 15;
        drawText(format("     = %.2f cycles per access", amat), Rect(12, y, W, 14), Fonts::mono(10, Weight::Bold), Theme::ink);
        y += 22;
        int total = 0;
        for (auto& [k, n] : h.stallCycles) total += n;
        drawText("Cycles lost waiting for memory: " + formatCount(total), Rect(12, y, W, 14), Fonts::small, Theme::secondary);
    }
}

void HierarchyCanvas::mouseDown(const MouseEvent& e) {
    Point p = localPoint(e);
    for (auto& [k, r] : cacheRects_)
        if (r.contains(p)) {
            if (onSelectCache) onSelectCache(k);
            return;
        }
}

// MARK: - Cache grid

void CacheGridCanvas::recompute(float w) {
    int sets = cache ? cache->config.sets() : 0;
    int ways = cache ? cache->config.ways : 1;
    size_ = Size(std::max(w, 120.0f + ways * 120.0f), 128 + sets * rowH + 20);
    setNeedsDisplay();
}

void CacheGridCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(dirty, Theme::panel);
    if (!cache) return;
    const Cache& c = *cache;
    const CacheConfig& cfg = c.config;
    drawText(cfg.name, Point(14, 8), Fonts::subheading, Theme::heading);
    std::string desc = cat(sizeText(cfg.size), " · ", cfg.blockSize, "-byte blocks · ", cfg.sets(), " sets × ", cfg.ways, " way",
                           cfg.ways == 1 ? "" : "s", " (", cfg.associativityName(), ") · ", replacementName(cfg.replacement), " · ",
                           lowercased(writePolicyName(cfg.write)));
    drawText(desc, Rect(14, 28, width() - 28, 14), Fonts::small, Theme::secondary);
    // Address breakdown of the last access.
    float ay = 50;
    if (c.last()) {
        const CacheEvent& e = *c.last();
        Cache::Split sp = c.split(e.address);
        struct Part {
            int n;
            const char* name;
            std::string val;
            Color color;
        };
        Part bits[] = {{cfg.tagBits(), "tag", hexString(sp.tag), Theme::fieldColor("rs1")},
                       {cfg.indexBits(), "set index", std::to_string(sp.set), Theme::fieldColor("funct3")},
                       {cfg.offsetBits(), "offset", std::to_string(sp.offset), Theme::fieldColor("rd")}};
        float total = width() - 28 - 92 - 200;
        float x = 14;
        drawText("last access", Point(x, ay + 1), Fonts::tiny, Theme::secondary);
        drawText(hexAddr(e.address), Point(x, ay + 14), Fonts::mono(11, Weight::Semibold), Theme::ink);
        x += 92;
        for (auto& b : bits) {
            if (b.n <= 0) continue;
            float w = std::max(78.0f, total * b.n / 32);
            Rect r{x, ay, w, 30};
            fillRect(r, b.color);
            strokeRect(r.insetBy(0.5f, 0.5f), Theme::borderStrong);
            drawText(cat(b.name, " (", b.n, " bits)"), Rect(r.minX() + 4, r.minY() + 2, w - 6, 12), Fonts::tiny, Theme::secondary);
            drawText(std::string(b.name) == "tag" ? "0x" + b.val : b.val, Rect(r.minX() + 4, r.minY() + 14, w - 6, 14), Fonts::mono(10.5f, Weight::Semibold),
                     Theme::text);
            x += w;
        }
        std::string verdict;
        if (e.hit) verdict = cat("HIT in way ", e.way);
        else {
            verdict = cat("MISS (", e.missKind ? missKindName(*e.missKind) : "", ")");
            if (e.evicted) verdict += e.writeback ? ", evicted a dirty block" : ", evicted a block";
        }
        drawText(verdict, Rect(x + 10, ay + 8, width() - x - 16, 16), Fonts::ui(11, Weight::Bold), e.hit ? Theme::green : Theme::crimson);
    } else {
        drawText("No accesses yet — step the program.", Point(14, ay + 7), Fonts::small, Theme::faint);
    }
    // Grid.
    float gy = 96;
    float setW = 46;
    float wayW = (width() - 28 - setW) / cfg.ways;
    drawText("set", Rect(14, gy, setW - 6, 14), Fonts::mono(9.5f, Weight::Semibold), Theme::faint, Align::Right);
    for (int w = 0; w < cfg.ways; w++)
        drawText(cat("way ", w), Rect(14 + setW + w * wayW, gy, wayW, 14), Fonts::mono(9.5f, Weight::Semibold), Theme::faint, Align::Center);
    float y0 = gy + 18;
    const std::optional<CacheEvent>& last = c.last();
    for (int s = 0; s < cfg.sets(); s++) {
        float y = y0 + s * rowH;
        if (y > dirty.maxY() + rowH || y + rowH < dirty.minY() - rowH) continue;
        bool isLastSet = last && last->set == s;
        if (isLastSet) fillRect({10, y, width() - 20, rowH}, Theme::selection);
        drawText(std::to_string(s), Rect(14, y, setW - 6, rowH), Fonts::mono(10, isLastSet ? Weight::Bold : Weight::Regular),
                 isLastSet ? Theme::text : Theme::secondary, Align::Right);
        for (int w = 0; w < cfg.ways; w++) {
            const Cache::Line& l = c.line(s, w);
            Rect r{14 + setW + w * wayW + 2, y + 2, wayW - 4, rowH - 4};
            if (!l.valid) {
                fillRounded(r, 3, Color::hex(0xF4F1EA));
                drawText("empty", r, Fonts::tiny, Theme::faint, Align::Center);
                continue;
            }
            int age = now - l.lastTouch;
            Color fill = Color::hex(0xE6EEF9);
            if (age <= 0) fill = (last && last->hit && isLastSet && last->way == w) ? Color::hex(0xBFE5CF) : Color::hex(0xFFD9A8);
            else if (age < 30) fill = Color::hex(0xE6EEF9).blended(0.5f * (1 - age / 30.0f), Color::hex(0xBFE5CF));
            fillRounded(r, 3, fill);
            if (isLastSet && last->way == w) strokeRounded(r, 3, last->hit ? Theme::green : Theme::crimson, 1.6f);
            uint32_t base = c.blockAddress(s, l.tag);
            std::string txt = wayW > 150 ? cat("tag ", hexString(l.tag), "  ·  ", format("%08x", base)) : format("%08x", base);
            drawText(txt, r.insetBy(5, 0), Fonts::mono(9.5f), Theme::text);
            if (l.dirty) drawText("D", Rect(r.maxX() - 14, r.minY(), 12, r.h), Fonts::mono(9, Weight::Bold), Theme::crimson, Align::Center);
        }
    }
}

// MARK: - Cache statistics

void CacheStatsCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panelAlt);
    line({0.5f, 0}, {0.5f, height()}, Theme::border);
    if (!cache) return;
    const CacheStats& s = cache->stats();
    float y = 10;
    drawText("Misses by cause (the 3 Cs)", Point(12, y - 2), Fonts::subheading, Theme::heading);
    y += 22;
    int total = std::max(1, s.misses);
    std::tuple<const char*, int, Color, const char*> causes[] = {{"Compulsory", s.compulsory, Theme::ink, "first touch of a block"},
                                                                 {"Capacity", s.capacity, Theme::amber, "cache too small for the working set"},
                                                                 {"Conflict", s.conflict, Theme::crimson, "too many blocks map to one set"}};
    for (auto& [name, n, color, note] : causes) {
        drawText(name, Point(12, y), Fonts::ui(11, Weight::Semibold), Theme::text);
        drawText(formatCount(n), Rect(12, y, width() - 24, 14), Fonts::mono(11, Weight::Semibold), color, Align::Right);
        y += 15;
        Rect bar{12, y, width() - 24, 6};
        fillRounded(bar, 3, Theme::border);
        fillRounded({bar.minX(), y, bar.w * n / total, 6}, 3, color);
        y += 9;
        drawText(note, Point(12, y), Fonts::tiny, Theme::secondary);
        y += 18;
    }
    drawText("Write-backs of dirty blocks: " + formatCount(s.writebacks), Point(12, y), Fonts::small, Theme::secondary);
    y += 24;
    line({10, y}, {width() - 10, y}, Theme::border);
    y += 8;
    drawText("Recent accesses", Point(12, y - 2), Fonts::subheading, Theme::heading);
    y += 20;
    int room = std::max(0, (int)((height() - y) / 15));
    int start = std::max(0, (int)recent.size() - room);
    for (int k = (int)recent.size() - 1; k >= start; k--) {
        const MemoryAccess& a = recent[k];
        Color c = a.servedBy == "L1" ? Theme::green : a.servedBy == "L2" ? Theme::amber : Theme::crimson;
        drawText(a.kind == AccessKind::Fetch ? "fetch" : accessKindName(a.kind), Point(12, y), Fonts::mono(9.5f), Theme::secondary);
        drawText(hex8(a.address), Point(54, y), Fonts::mono(9.5f), Theme::text);
        drawText(a.servedBy == "L1" ? std::string("hit") : cat(a.servedBy, " +", a.penalty), Rect(12, y, width() - 24, 13), Fonts::mono(9.5f, Weight::Semibold), c,
                 Align::Right);
        y += 15;
    }
}

// MARK: - Memory view

MemoryView::MemoryView() {
    for (View* v : std::initializer_list<View*>{&hierarchy, &gridPanel, &stats, &which, &sizeP, &blockP, &waysP, &replP, &writeP, &enabled, &l2On, &empty})
        addSubview(v);
    replP.set({"LRU", "FIFO", "Random"}, 0);
    writeP.set({writePolicyName(WritePolicy::WriteBack), writePolicyName(WritePolicy::WriteThrough)}, 0);
    which.onChange = [this](int k) {
        selected_ = k;
        hierarchy.selectedCache = k;
        if (model) reload(*model, ModelEvent::Config);
    };
    hierarchy.onSelectCache = [this](int k) {
        selected_ = k;
        which.setSelected(k);
        hierarchy.selectedCache = k;
        if (model) reload(*model, ModelEvent::Config);
    };
    sizeP.onChange = [this](int k) { edit([&](CacheConfig& c) { c.size = sizes_[k]; }); };
    blockP.onChange = [this](int k) { edit([&](CacheConfig& c) { c.blockSize = blocks_[k]; }); };
    waysP.onChange = [this](int k) {
        // The menu lists only the way counts that fit.
        int ways = 1;
        int idx = 0;
        if (model && model->pipeline()) {
            const MemoryHierarchy& h = model->pipeline()->hierarchy;
            const Cache& cache = selected_ == 0 ? h.l1i : selected_ == 1 ? h.l1d : *h.l2;
            for (int w : waysList_)
                if (w <= cache.config.blocks()) {
                    if (idx == k) ways = w;
                    idx++;
                }
        }
        edit([&](CacheConfig& c) { c.ways = ways; });
    };
    replP.onChange = [this](int k) { edit([&](CacheConfig& c) { c.replacement = (Replacement)k; }); };
    writeP.onChange = [this](int k) { edit([&](CacheConfig& c) { c.write = (WritePolicy)k; }); };
    enabled.onChange = [this](bool on) {
        if (!model) return;
        PipelineConfig pc = model->pipelineConfig();
        HierarchyConfig h = pc.hierarchy;
        h.enabled = on;
        if (on && h.dramLatency == 0) h = HierarchyConfig::teaching();
        pc.hierarchy = h;
        model->setPipelineConfig(pc);
    };
    l2On.onChange = [this](bool on) {
        if (!model) return;
        PipelineConfig pc = model->pipelineConfig();
        pc.hierarchy.l2 = on ? HierarchyConfig::teaching().l2 : std::nullopt;
        if (!on && selected_ == 2) selected_ = 1;
        model->setPipelineConfig(pc);
    };
}

void MemoryView::edit(const std::function<void(CacheConfig&)>& f) {
    if (!model) return;
    PipelineConfig pc = model->pipelineConfig();
    HierarchyConfig& h = pc.hierarchy;
    CacheConfig c = selected_ == 0 ? h.l1i : selected_ == 1 ? h.l1d : (h.l2 ? *h.l2 : *HierarchyConfig::teaching().l2);
    f(c);
    // Keep the configuration legal: blocks fit, ways fit.
    if (c.blockSize > c.size) c.blockSize = c.size;
    while (c.ways > c.size / c.blockSize) c.ways /= 2;
    switch (selected_) {
    case 0: h.l1i = c; break;
    case 1: h.l1d = c; break;
    default: h.l2 = c; break;
    }
    model->setPipelineConfig(pc);
}

void MemoryView::layout() {
    float hw = 270, sw = 220;
    hierarchy.setFrame({0, 0, hw, height()});
    float cx = hw + 10;
    Size ws = which.intrinsicSize();
    which.setFrame({cx, 8, ws.w, 24});
    enabled.setFrame({which.frame().maxX() + 14, 10, enabled.intrinsicSize().w, 20});
    l2On.setFrame({enabled.frame().maxX() + 10, 10, l2On.intrinsicSize().w, 20});
    float x = cx;
    std::pair<PopUp*, float> pops[] = {{&sizeP, 92}, {&blockP, 88}, {&waysP, 104}, {&replP, 86}, {&writeP, 250}};
    for (auto& [p, w] : pops) {
        p->setFrame({x, 38, w, 26});
        x += w + 6;
    }
    gridPanel.setFrame({hw, 70, width() - hw - sw, height() - 70});
    stats.setFrame({width() - sw, 70, sw, height() - 70});
    grid.recompute(gridPanel.width() - 4);
    empty.setFrame(bounds());
}

void MemoryView::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    line({270, 69.5f}, {width(), 69.5f}, Theme::border);
}

void MemoryView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    hierarchy.model = &m;
    Pipeline* p = m.pipeline();
    if (!p) {
        empty.setHidden(false);
        empty.title = "No program running";
        empty.message = "Fix the errors in the program first.";
        empty.setNeedsDisplay();
        return;
    }
    empty.setHidden(true);
    const MemoryHierarchy& h = p->hierarchy;
    if (!h.l2 && selected_ == 2) selected_ = 1;
    which.setSelected(selected_);
    which.setEnabledMask(std::vector<bool>{true, true, h.l2 != nullptr});
    hierarchy.selectedCache = selected_;
    enabled.setOn(h.config.enabled);
    l2On.setOn(h.l2 != nullptr);
    const Cache& cache = selected_ == 0 ? h.l1i : selected_ == 1 ? h.l1d : *h.l2;
    const CacheConfig& cfg = cache.config;
    auto indexOf = [](const std::vector<int>& v, int x, int fallback) {
        auto it = std::find(v.begin(), v.end(), x);
        return it == v.end() ? fallback : (int)(it - v.begin());
    };
    std::vector<std::string> sizeNames, blockNames, wayNames;
    for (int s : sizes_) sizeNames.push_back("size " + sizeText(s));
    for (int b : blocks_) blockNames.push_back(cat("block ", b, " B"));
    std::vector<int> wl;
    for (int w : waysList_)
        if (w <= cfg.blocks()) wl.push_back(w);
    for (int w : wl) wayNames.push_back(w == 1 ? std::string("direct-mapped") : cat(w, "-way"));
    sizeP.set(sizeNames, indexOf(sizes_, cfg.size, 3));
    blockP.set(blockNames, indexOf(blocks_, cfg.blockSize, 2));
    waysP.set(wayNames, indexOf(wl, cfg.ways, 0));
    replP.select((int)cfg.replacement);
    writeP.select((int)cfg.write);
    for (PopUp* v : {&sizeP, &blockP, &waysP, &replP, &writeP}) v->setEnabled(h.config.enabled);
    grid.cache = h.config.enabled ? &cache : nullptr;
    grid.now = p->cycle();
    grid.recompute(gridPanel.width() - 4);
    gridPanel.setNeedsLayout();
    if (cache.last() && !m.running()) {
        gridPanel.layoutSubtreeIfNeeded();
        float y = 114 + cache.last()->set * grid.rowH;
        grid.scrollToVisible({0, y - 30, 10, 80});
    }
    stats.cache = h.config.enabled ? &cache : nullptr;
    stats.recent.clear();
    for (auto& a : h.recent) {
        bool keep = selected_ == 0 ? a.kind == AccessKind::Fetch : selected_ == 1 ? a.kind != AccessKind::Fetch : a.servedBy != "L1";
        if (keep) stats.recent.push_back(a);
    }
    hierarchy.setNeedsDisplay();
    grid.setNeedsDisplay();
    stats.setNeedsDisplay();
}

}  // namespace ui
