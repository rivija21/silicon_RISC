// One run of the whole tool chain, keeping every intermediate result.
#pragma once

#include "Interpreter.h"

namespace sp {

struct CompileOptions {
    std::set<PassID> passes;
    /// Pretend the machine has only this many allocatable registers (to watch spilling).
    std::optional<int> registerLimit;
    /// Compiling a library member: no main needed, library names may be defined.
    bool isLibrary = false;

    static CompileOptions o0() { return CompileOptions{{}, std::nullopt, false}; }
    static CompileOptions o1() {
        CompileOptions o;
        o.passes = std::set<PassID>(allPasses().begin(), allPasses().end());
        return o;
    }
    bool operator==(const CompileOptions&) const = default;
};

/// One run of the whole tool chain, keeping every intermediate result so
/// the app can show each stage: tokens → syntax tree → symbols → IR (after
/// each pass) → register allocation → assembly → object file → executable.
class Compilation {
public:
    const std::string source;
    const CompileOptions options;
    const std::string name;
    const LineTable lines;
    bool isAssembly = false;

    std::optional<LexResult> lex;
    ProgramP program;
    SemaResultP sema;
    std::vector<PassSnapshot> snapshots;
    IRModuleP ir;
    std::map<std::string, Allocation> allocations;
    std::vector<FrameLayout> frames;
    std::vector<AsmLine> asmLines;
    ObjectFileP object;
    ExecutableP executable;
    std::vector<Diagnostic> diagnostics;
    /// The first stage that failed, if any.
    std::optional<Stage> failedStage;
    std::vector<std::pair<Stage, double>> timings;

    std::vector<Diagnostic> errors() const;
    bool succeeded() const { return executable != nullptr; }

    /// Compiles a C program and links it into an executable.
    static std::shared_ptr<Compilation> compile(const std::string& source, const CompileOptions& options = CompileOptions::o1());
    /// Assembles and links a hand-written assembly program.
    static std::shared_ptr<Compilation> assemble(const std::string& source);
    /// Runs everything up to and including the assembler.
    static std::shared_ptr<Compilation> compileObject(const std::string& source, const std::string& name, const CompileOptions& options);

    /// Assembly lines generated for a C source line.
    std::vector<int> asmLinesForSourceLine(int line) const;

    Compilation(std::string src, std::string nm, CompileOptions opts);

private:
    void fail(Stage s) {
        if (!failedStage) failedStage = s;
    }
    void link(const ObjectFileP& obj);
    void describeLocations(const Program& prog, const IRModule& m);
};
using CompilationP = std::shared_ptr<Compilation>;

/// The start-up code and the small C library every program is linked with.
namespace Runtime {
extern const char* const crt0;
extern const char* const syscalls;
struct Member {
    const char* name;
    const char* summary;
    const char* source;
};
const std::vector<Member>& cMembers();

struct Library {
    ObjectFileP crt0;
    std::vector<ObjectFileP> members;
    std::map<std::string, CompilationP> compilations;
};
/// The library, built once: assembly members assembled, C members compiled with every optimisation on.
const Library& library();
}  // namespace Runtime

}  // namespace sp
