#include "Compiler.h"

#include <chrono>

namespace sp {

Compilation::Compilation(std::string src, std::string nm, CompileOptions opts)
    : source(std::move(src)), options(std::move(opts)), name(std::move(nm)), lines(source) {}

std::vector<Diagnostic> Compilation::errors() const {
    std::vector<Diagnostic> out;
    for (auto& d : diagnostics)
        if (d.severity == Diagnostic::Severity::Error) out.push_back(d);
    return out;
}

template <class F>
static auto timed(std::vector<std::pair<Stage, double>>& timings, Stage s, F&& body) {
    auto t0 = std::chrono::steady_clock::now();
    auto r = body();
    timings.push_back({s, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count()});
    return r;
}

CompilationP Compilation::compile(const std::string& source, const CompileOptions& options) {
    auto c = compileObject(source, "main.o", options);
    if (!c->object || c->failedStage) return c;
    c->link(c->object);
    return c;
}

CompilationP Compilation::assemble(const std::string& source) {
    auto c = std::make_shared<Compilation>(source, "main.o", CompileOptions::o1());
    c->isAssembly = true;
    ObjectFileP obj = timed(c->timings, Stage::Assembler, [&] { return Assembler::assembleText(source, "main.o"); });
    c->object = obj;
    c->asmLines = obj->lines;
    c->diagnostics.insert(c->diagnostics.end(), obj->diagnostics.begin(), obj->diagnostics.end());
    if (obj->hasErrors()) {
        c->fail(Stage::Assembler);
        return c;
    }
    auto m = obj->symbols.find("main");
    if (m != obj->symbols.end()) {
        auto gs = obj->globalSymbols();
        if (std::find(gs.begin(), gs.end(), "main") == gs.end()) m->second.isGlobal = true;
    }
    c->link(obj);
    return c;
}

void Compilation::link(const ObjectFileP& obj) {
    const Runtime::Library& lib = Runtime::library();
    auto [exe, d] = timed(timings, Stage::Linker, [&] { return Linker::link({lib.crt0, obj}, lib.members); });
    diagnostics.insert(diagnostics.end(), d.begin(), d.end());
    if (!exe) fail(Stage::Linker);
    executable = exe;
}

CompilationP Compilation::compileObject(const std::string& source, const std::string& name, const CompileOptions& options) {
    auto c = std::make_shared<Compilation>(source, name, options);
    LexResult lx = timed(c->timings, Stage::Lexer, [&] { return sp::lex(source); });
    c->lex = lx;
    c->diagnostics.insert(c->diagnostics.end(), lx.diagnostics.begin(), lx.diagnostics.end());
    if (lx.hasErrors()) {
        c->fail(Stage::Lexer);
        return c;
    }

    auto [prog, pd] = timed(c->timings, Stage::Parser, [&] { return parse(*c->lex); });
    c->diagnostics.insert(c->diagnostics.end(), pd.begin(), pd.end());
    if (!prog) {
        c->fail(Stage::Parser);
        return c;
    }
    c->program = prog;

    SemaResultP sema = timed(c->timings, Stage::Semantic, [&] { return analyze(*prog, options.isLibrary); });
    c->sema = sema;
    c->diagnostics.insert(c->diagnostics.end(), sema->diagnostics.begin(), sema->diagnostics.end());
    if (sema->hasErrors()) {
        c->fail(Stage::Semantic);
        return c;
    }

    auto ids = std::make_shared<IDGen>();
    auto snaps = timed(c->timings, Stage::IR, [&] {
        auto m = lower(*prog, *sema, ids);
        return Optimizer::run(*m, options.passes);
    });
    c->snapshots = snaps;
    IRModuleP final = snaps.back().module;
    c->ir = final;

    CodeGen gen(source);
    c->asmLines = timed(c->timings, Stage::Codegen, [&] {
        for (auto& f : final->functions) c->allocations[f->name] = allocateRegisters(*f, options.registerLimit);
        return gen.generate(*final, c->allocations);
    });
    c->frames = gen.frames();
    c->describeLocations(*prog, *final);

    ObjectFileP obj = timed(c->timings, Stage::Assembler, [&] { return Assembler::assemble(c->asmLines, name); });
    c->object = obj;
    c->diagnostics.insert(c->diagnostics.end(), obj->diagnostics.begin(), obj->diagnostics.end());
    if (obj->hasErrors()) c->fail(Stage::Assembler);
    return c;
}

/// Fills Symbol.location ("stack: -20(s0)", "register s1"…) for the symbol-table view.
void Compilation::describeLocations(const Program& prog, const IRModule& m) {
    for (auto& fd : prog.functions()) {
        const IRFunction* f = nullptr;
        for (auto& x : m.functions)
            if (x->name == fd->name) {
                f = x.get();
                break;
            }
        auto a = allocations.find(fd->name);
        const FrameLayout* fr = nullptr;
        for (auto& x : frames)
            if (x.function == fd->name) {
                fr = &x;
                break;
            }
        if (!f || a == allocations.end() || !fr) continue;
        for (auto& s : fd->locals) {
            int k = -1;
            for (int i = 0; i < (int)f->slots.size(); i++)
                if (f->slots[i].symbolID && *f->slots[i].symbolID == s->id) {
                    k = i;
                    break;
                }
            if (k >= 0 && k < (int)fr->slotOffset.size()) {
                s->location = cat("stack: ", fr->slotOffset[k], "(s0)");
                continue;
            }
            int v = -1;
            for (int i = 0; i < (int)f->vregs.size(); i++)
                if (f->vregs[i].name == "%" + s->uniqueName && f->vregs[i].isVariable) {
                    v = i;
                    break;
                }
            if (v < 0) {
                s->location = "optimised away";
                continue;
            }
            const Location* loc = a->second.locationOf(v);
            if (!loc) s->location = "optimised away";
            else if (loc->kind == Location::Register) s->location = "register " + Reg::name(loc->n);
            else s->location = loc->n < (int)fr->spillOffset.size() ? cat("spilled: ", fr->spillOffset[loc->n], "(s0)") : "spilled";
        }
    }
    for (auto& g : prog.globals())
        if (g->symbol) g->symbol->location = ".data (" + g->symbol->name + ")";
}

std::vector<int> Compilation::asmLinesForSourceLine(int line) const {
    std::vector<int> out;
    for (int i = 0; i < (int)asmLines.size(); i++)
        if (asmLines[i].origin && asmLines[i].origin->line == line && asmLines[i].kind == AsmLine::Kind::Instruction) out.push_back(i);
    return out;
}

}  // namespace sp
