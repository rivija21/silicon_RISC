#include "AppModel.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "Gfx.h"

namespace ui {

using namespace sp;

enum TimerID { CompileTimer = 1, ClockTimer = 2 };

// MARK: - Settings

static std::string escape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

static std::string unescape(const std::string& s) {
    std::string out;
    for (size_t k = 0; k < s.size(); k++) {
        if (s[k] == '\\' && k + 1 < s.size()) {
            char n = s[++k];
            out += n == 'n' ? '\n' : n == 'r' ? '\r' : n;
        } else {
            out += s[k];
        }
    }
    return out;
}

Settings& Settings::shared() {
    static Settings s;
    return s;
}

std::wstring Settings::path() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) dir = appData;
    CoTaskMemFree(appData);
    if (dir.empty()) return L"SiliconPath-settings.ini";
    return dir + L"\\SiliconPath\\settings.ini";
}

Settings::Settings() {
    std::ifstream f(std::filesystem::path(path()), std::ios::binary);
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        values_[line.substr(0, eq)] = unescape(line.substr(eq + 1));
    }
    loaded_ = true;
}

std::optional<std::string> Settings::string(const std::string& key) const {
    auto it = values_.find(key);
    if (it == values_.end()) return std::nullopt;
    return it->second;
}

void Settings::set(const std::string& key, const std::string& value) { values_[key] = value; }

void Settings::save() {
    std::filesystem::path p(path());
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    for (auto& [k, v] : values_) f << k << '=' << escape(v) << '\n';
}

// MARK: - Model

AppModel::AppModel(Scheduler* scheduler, bool persist) : scheduler_(scheduler), persist_(persist) {
    Settings& d = Settings::shared();
    const Example& first = Examples::all()[0];
    auto get = [&](const char* k) { return persist ? d.string(k) : std::nullopt; };
    source_ = get("source").value_or(first.source);
    isAssembly_ = get("isAssembly").value_or("0") == "1";
    exampleName = get("example").value_or(first.name);
    CompileOptions opts = CompileOptions::o1();
    if (auto p = get("passes")) {
        opts.passes.clear();
        for (auto& name : split(*p, ','))
            if (auto id = passFromRaw(name)) opts.passes.insert(*id);
    }
    if (auto l = get("registerLimit")) {
        int limit = (int)parseInt(*l).value_or(0);
        opts.registerLimit = limit > 0 ? std::optional<int>(limit) : std::nullopt;
    }
    options_ = opts;
    PipelineConfig pc;
    pc.forwarding = get("forwarding").value_or("1") == "1";
    if (auto b = get("branchStage"))
        for (auto s : allBranchStages())
            if (*b == branchStageName(s)) pc.branchStage = s;
    if (auto p = get("predictor"))
        for (auto k : allPredictors())
            if (*p == predictorName(k)) pc.predictor = k;
    pipelineConfig_ = pc;
    if (auto s = get("station")) {
        int k = (int)parseInt(*s).value_or(0);
        if (k >= 0 && k < sp::stationCount) station_ = (Station)k;
    }
    if (auto s = get("speed")) speed = std::strtod(s->c_str(), nullptr);
    if (speed <= 0) speed = 12;
}

void AppModel::save() {
    if (!persist_) return;
    Settings& d = Settings::shared();
    d.set("source", source_);
    d.set("isAssembly", isAssembly_ ? "1" : "0");
    d.set("example", exampleName);
    std::vector<std::string> ps;
    for (PassID p : options_.passes) ps.push_back(passRawValue(p));
    d.set("passes", join(ps, ","));
    d.set("registerLimit", std::to_string(options_.registerLimit.value_or(0)));
    d.set("forwarding", pipelineConfig_.forwarding ? "1" : "0");
    d.set("branchStage", branchStageName(pipelineConfig_.branchStage));
    d.set("predictor", predictorName(pipelineConfig_.predictor));
    d.set("station", std::to_string((int)station_));
    d.set("speed", format("%g", speed));
    d.save();
}

void AppModel::notify(ModelEvent e) {
    for (size_t k = 0; k < observers_.size(); k++) observers_[k](e);
}

void AppModel::setOptions(const CompileOptions& o) {
    if (o == options_) return;
    options_ = o;
    scheduleCompile(0);
}

void AppModel::setPipelineConfig(const PipelineConfig& c) {
    if (c == pipelineConfig_) return;
    pipelineConfig_ = c;
    resetMachine();
    notify(ModelEvent::Config);
}

void AppModel::setSelection(const Selection& s) {
    if (s == selection_) return;
    selection_ = s;
    notify(ModelEvent::Selection);
}

void AppModel::setIRSnapshot(std::optional<int> s) {
    irSnapshot_ = s;
    notify(ModelEvent::Selection);
}

void AppModel::setFocusFunction(std::optional<std::string> f) {
    if (f == focusFunction_) return;
    focusFunction_ = std::move(f);
    notify(ModelEvent::Selection);
}

// MARK: Source and compiling

void AppModel::setSource(const std::string& s, bool fromEditor) {
    if (s == source_) return;
    source_ = s;
    scheduleCompile(fromEditor ? 0.45 : 0);
}

void AppModel::load(const Example& ex) {
    pause();
    exampleName = ex.name;
    isAssembly_ = ex.isAssembly;
    source_ = ex.source;
    selection_ = Selection();
    irSnapshot_.reset();
    focusFunction_.reset();
    notify(ModelEvent::Selection);
    compileNow();
    setStation(ex.isAssembly && stationNeedsC(ex.station) ? Station::Asm : ex.station);
}

void AppModel::loadFile(const std::wstring& path) {
    std::ifstream f(std::filesystem::path(path), std::ios::binary);
    if (!f) return;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) s = s.substr(3);
    s = replaceAll(s, "\r\n", "\n");
    pause();
    std::filesystem::path p(path);
    exampleName = narrow(p.filename().wstring());
    std::string ext = lowercased(narrow(p.extension().wstring()));
    isAssembly_ = ext == ".s" || ext == ".asm";
    source_ = s;
    selection_ = Selection();
    notify(ModelEvent::Selection);
    compileNow();
}

void AppModel::setAssemblyMode(bool on) {
    if (on == isAssembly_) return;
    isAssembly_ = on;
    bool moved = on && stationNeedsC(station_);
    if (moved) station_ = Station::Asm;
    compileNow();
    if (moved) notify(ModelEvent::Station);
}

void AppModel::scheduleCompile(double delay) {
    if (!scheduler_) {
        compileNow();
        return;
    }
    scheduler_->schedule(CompileTimer, delay, [this] { compileNow(); });
}

void AppModel::compileNow() {
    if (scheduler_) scheduler_->cancel(CompileTimer);
    CompilationP c = isAssembly_ ? Compilation::assemble(source_) : Compilation::compile(source_, options_);
    compilation_ = c;
    if (c->succeeded()) {
        good_ = c;
        resetMachine();
    } else if (!good_ || good_->isAssembly != isAssembly_) {
        good_.reset();
        pipeline_.reset();
    }
    if (irSnapshot_ && *irSnapshot_ >= (good_ ? (int)good_->snapshots.size() : 0)) irSnapshot_.reset();
    notify(ModelEvent::Compiled);
    save();
}

CompilationP AppModel::shown() const {
    if (compilation_ && compilation_->succeeded()) return compilation_;
    return good_ ? good_ : compilation_;
}

bool AppModel::hasErrors() const { return compilation_ && !compilation_->errors().empty(); }

// MARK: Station

void AppModel::setStation(Station s) {
    if (s == station_) return;
    station_ = s;
    notify(ModelEvent::Station);
    save();
}

// MARK: Running the machine

void AppModel::resetMachine() {
    pause();
    if (good_ && good_->executable) pipeline_ = std::make_unique<Pipeline>(good_->executable, pipelineConfig_);
    else pipeline_.reset();
    notify(ModelEvent::Stepped);
}

void AppModel::stepCycle(int n) {
    Pipeline* p = pipeline_.get();
    if (!p || p->halted()) return;
    for (int k = 0; k < n; k++)
        if (!p->step()) break;
    notify(ModelEvent::Stepped);
    if (p->halted()) pause();
}

/// Steps until the next instruction completes (leaves WB).
void AppModel::stepInstruction() {
    Pipeline* p = pipeline_.get();
    if (!p || p->halted()) return;
    int target = p->stats().retired + 1;
    int guardCount = 0;
    while (p->stats().retired < target && p->step() && guardCount < 10000) guardCount += 1;
    notify(ModelEvent::Stepped);
}

/// Steps until an instruction from a different source line completes.
void AppModel::stepLine() {
    Pipeline* p = pipeline_.get();
    if (!p || p->halted() || !good_ || !good_->executable) return;
    const Executable& exe = *good_->executable;
    auto lineOf = [&](std::optional<int> seq) -> std::optional<int> {
        if (!seq) return std::nullopt;
        auto pc = p->seqPC(*seq);
        if (!pc) return std::nullopt;
        const LinkedInstruction* ins = exe.instruction(*pc);
        return ins ? sourceLine(*ins) : std::nullopt;
    };
    auto retired = [&]() -> std::optional<int> { return p->lastRecord() ? p->lastRecord()->retired : std::nullopt; };
    int startLine = lineOf(retired()).value_or(-1);
    int n = 0;
    while (p->step() && n < 200000) {
        n += 1;
        auto l = lineOf(retired());
        if (l && *l > 0 && *l != startLine) break;
    }
    notify(ModelEvent::Stepped);
}

void AppModel::stepBack() {
    Pipeline* p = pipeline_.get();
    if (!p || p->cycle() == 0 || !good_ || !good_->executable) return;
    pause();
    int target = p->cycle() - 1;
    auto q = std::make_unique<Pipeline>(good_->executable, pipelineConfig_);
    while (q->cycle() < target && q->step()) {}
    pipeline_ = std::move(q);
    notify(ModelEvent::Stepped);
}

void AppModel::runToEnd() {
    Pipeline* p = pipeline_.get();
    if (!p || p->halted()) return;
    pause();
    auto t0 = std::chrono::steady_clock::now();
    while (!p->halted() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4)) {
        for (int k = 0; k < 20000; k++)
            if (!p->step()) break;
    }
    notify(ModelEvent::Stepped);
}

void AppModel::togglePlay() { running_ ? pause() : play(); }

void AppModel::play() {
    Pipeline* p = pipeline_.get();
    if (!p || p->halted() || !scheduler_) return;
    running_ = true;
    carry_ = 0;
    lastTick_ = std::chrono::steady_clock::now();
    scheduler_->repeat(ClockTimer, 1.0 / 30, [this] { tick(); });
    notify(ModelEvent::Running);
}

void AppModel::pause() {
    if (!running_) return;
    running_ = false;
    if (scheduler_) scheduler_->cancel(ClockTimer);
    notify(ModelEvent::Running);
}

void AppModel::tick() {
    Pipeline* p = pipeline_.get();
    if (!p) {
        pause();
        return;
    }
    auto now = std::chrono::steady_clock::now();
    carry_ += std::chrono::duration<double>(now - lastTick_).count() * speed;
    lastTick_ = now;
    int n = (int)carry_;
    carry_ -= n;
    if (n > 0) {
        auto t0 = std::chrono::steady_clock::now();
        int k = 0;
        while (k < n && p->step()) {
            k += 1;
            if ((k & 1023) == 0 && std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(25)) break;
        }
        notify(ModelEvent::Stepped);
    }
    if (p->halted()) pause();
}

// MARK: Lookups

std::optional<int> AppModel::sourceLine(const LinkedInstruction& ins) const {
    if (ins.object != "main.o") return std::nullopt;
    if (good_ && good_->isAssembly) return ins.asmLine + 1;
    if (ins.origin && ins.origin->line > 0) return ins.origin->line;
    return std::nullopt;
}

std::optional<int> AppModel::lineOfPC(uint32_t pc) const {
    if (!good_ || !good_->executable) return std::nullopt;
    const LinkedInstruction* ins = good_->executable->instruction(pc);
    return ins ? sourceLine(*ins) : std::nullopt;
}

std::map<int, std::vector<int>> AppModel::stagesByLine() const {
    std::map<int, std::vector<int>> out;
    Pipeline* p = pipeline_.get();
    if (!p || !p->lastRecord() || !good_ || !good_->executable) return out;
    const CycleRecord& rec = *p->lastRecord();
    for (int k = 0; k < 5; k++) {
        const StageSlot& s = rec.stages[k];
        if (s.seq < 0 || s.status == StageSlot::Status::Flushed) continue;
        if (const LinkedInstruction* ins = good_->executable->instruction(s.pc))
            if (auto l = sourceLine(*ins)) out[*l].push_back(k);
    }
    return out;
}

void AppModel::selectLine(std::optional<int> line, Station origin) {
    Selection s;
    s.line = line;
    s.origin = origin;
    setSelection(s);
}

}  // namespace ui
