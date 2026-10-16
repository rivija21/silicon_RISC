// The app's state: the program, its compilation, the running machine,
// the station being shown and the current selection. Views observe it.
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../core/SiliconCore.h"
#include "Station.h"

namespace ui {

using sp::Station;

struct Selection {
    std::optional<int> line;
    std::optional<int> node;
    std::optional<uint32_t> pc;
    std::optional<int> irInst;
    /// Source text to select in the editor.
    std::optional<sp::SourceRange> range;
    /// Which view made the selection, so it does not scroll itself.
    std::optional<Station> origin;
    bool operator==(const Selection&) const = default;
};

enum class ModelEvent { Compiled, Stepped, Selection, Station, Config, Running };

/// Timers, provided by the window.
class Scheduler {
public:
    virtual ~Scheduler() = default;
    /// Runs f once after `seconds`, replacing an earlier timer with the same id.
    virtual void schedule(int id, double seconds, std::function<void()> f) = 0;
    /// Calls f every `seconds` until cancelled.
    virtual void repeat(int id, double seconds, std::function<void()> f) = 0;
    virtual void cancel(int id) = 0;
};

/// A tiny persistent key-value store (the counterpart of UserDefaults).
class Settings {
public:
    static Settings& shared();
    std::optional<std::string> string(const std::string& key) const;
    void set(const std::string& key, const std::string& value);
    void save();
    /// Where the settings live (%APPDATA%\SiliconPath\settings.ini).
    static std::wstring path();

private:
    Settings();
    std::map<std::string, std::string> values_;
    bool loaded_ = false;
};

class AppModel {
public:
    explicit AppModel(Scheduler* scheduler, bool persist = true);

    const std::string& source() const { return source_; }
    bool isAssembly() const { return isAssembly_; }
    const sp::CompileOptions& options() const { return options_; }
    void setOptions(const sp::CompileOptions& o);
    const sp::PipelineConfig& pipelineConfig() const { return pipelineConfig_; }
    void setPipelineConfig(const sp::PipelineConfig& c);
    sp::CompilationP compilation() const { return compilation_; }
    /// The last compilation that produced an executable (views keep showing it while you type an error).
    sp::CompilationP good() const { return good_; }
    sp::Pipeline* pipeline() const { return pipeline_.get(); }
    Station station() const { return station_; }
    const Selection& selection() const { return selection_; }
    void setSelection(const Selection& s);
    std::optional<int> irSnapshot() const { return irSnapshot_; }
    void setIRSnapshot(std::optional<int> s);
    std::optional<std::string> focusFunction() const { return focusFunction_; }
    void setFocusFunction(std::optional<std::string> f);
    bool running() const { return running_; }
    /// Clock speed while running, in cycles per second.
    double speed = 12;
    std::string exampleName;

    void save();
    void observe(std::function<void(ModelEvent)> f) { observers_.push_back(std::move(f)); }
    void notify(ModelEvent e);

    // Source and compiling.
    void setSource(const std::string& s, bool fromEditor = true);
    void load(const sp::Example& ex);
    void loadFile(const std::wstring& path);
    void setAssemblyMode(bool on);
    void scheduleCompile(double delay);
    void compileNow();
    /// The compilation the views should display.
    sp::CompilationP shown() const;
    bool hasErrors() const;

    // Station.
    void setStation(Station s);

    // Running the machine.
    void resetMachine();
    void stepCycle(int n = 1);
    void stepInstruction();
    void stepLine();
    void stepBack();
    void runToEnd();
    void togglePlay();
    void play();
    void pause();

    // Lookups.
    /// Source line (C, or the assembly line in assembly mode) an instruction came from.
    std::optional<int> sourceLine(const sp::LinkedInstruction& ins) const;
    std::optional<int> lineOfPC(uint32_t pc) const;
    /// For every source line with an instruction in the pipeline right now: which stages.
    std::map<int, std::vector<int>> stagesByLine() const;
    void selectLine(std::optional<int> line, Station origin);

private:
    Scheduler* scheduler_;
    bool persist_;
    std::string source_;
    bool isAssembly_ = false;
    sp::CompileOptions options_;
    sp::PipelineConfig pipelineConfig_;
    sp::CompilationP compilation_;
    sp::CompilationP good_;
    std::unique_ptr<sp::Pipeline> pipeline_;
    Station station_ = Station::Source;
    Selection selection_;
    std::optional<int> irSnapshot_;
    std::optional<std::string> focusFunction_;
    bool running_ = false;
    std::vector<std::function<void(ModelEvent)>> observers_;
    double carry_ = 0;
    std::chrono::steady_clock::time_point lastTick_;
    void tick();
};

}  // namespace ui
