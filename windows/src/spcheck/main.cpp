// spcheck — the compiler's test driver.
//
//   spcheck hand            hand-written programs, checked against the native C compiler
//   spcheck fuzz N [seed]   N random programs, differential testing
//   spcheck native N [seed] random programs also checked against the native C compiler
//   spcheck show seed       print one generated program
//   spcheck file x.c        compile and run one file in every configuration
//   spcheck asm x.c [O0]    print the generated assembly
//   spcheck verify          compile and run every built-in example on both CPU models
//
// The native compiler is `gcc` from PATH (set SPCHECK_CC to use another one, e.g. clang).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../core/SiliconCore.h"
#include "../ui/Station.h"
#include "Tests.h"

using namespace sp;

namespace {

std::wstring widen(const std::string& s) {
    std::u16string u = toUTF16(s);
    return std::wstring(u.begin(), u.end());
}

std::string narrow(const std::wstring& s) { return toUTF8(std::u16string(s.begin(), s.end())); }

std::string envOr(const char* name, const std::string& fallback) {
    wchar_t buf[2048];
    DWORD n = GetEnvironmentVariableW(widen(name).c_str(), buf, 2048);
    if (n == 0 || n >= 2048) return fallback;
    return narrow(std::wstring(buf, n));
}

std::string tempDir() {
    std::string t = envOr("SPCHECK_TMP", "");
    if (!t.empty()) return t;
    wchar_t buf[MAX_PATH + 1];
    DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    std::string p = narrow(std::wstring(buf, n));
    if (!p.empty() && (p.back() == '\\' || p.back() == '/')) p.pop_back();
    return p;
}

std::string readFile(const std::string& path) {
    std::ifstream f(std::filesystem::path(widen(path)), std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool writeFile(const std::string& path, const std::string& data) {
    std::ofstream f(std::filesystem::path(widen(path)), std::ios::binary);
    f << data;
    return (bool)f;
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

/// Runs a command line, sending stdout and stderr to files. Returns the exit code, or nullopt on failure/timeout.
std::optional<DWORD> runProcess(const std::string& cmdline, const std::string& outFile, const std::string& errFile, DWORD timeoutMs) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE hOut = CreateFileW(widen(outFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE hErr = CreateFileW(widen(errFile).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hOut == INVALID_HANDLE_VALUE || hErr == INVALID_HANDLE_VALUE) return std::nullopt;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = hOut;
    si.hStdError = hErr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = widen(cmdline);
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOut);
    CloseHandle(hErr);
    if (!ok) return std::nullopt;
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (w == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (w == WAIT_TIMEOUT) return std::nullopt;
    return code;
}

const char* prelude = R"SRC(#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
__attribute__((constructor)) static void sp_binary_stdout(void) { _setmode(_fileno(stdout), _O_BINARY); }
#endif
static void print_int(int x) { printf("%d", x); }
static void print_uint(unsigned x) { printf("%u", x); }
static void print_hex(unsigned x) { printf("0x%08x", x); }
static void print_char(int c) { putchar(c); }
static void print_str(char *s) { fputs(s, stdout); }
static unsigned sp_next = 1;
static int sp_rand(void) { sp_next = sp_next * 1103515245u + 12345u; return (int)((sp_next >> 16) & 32767u); }
static void sp_srand(unsigned s) { sp_next = s; }
#define rand sp_rand
#define srand sp_srand
#define bsearch sp_bsearch

)SRC";

/// Every way of building and running a program that must agree.
struct Variant {
    std::string name;
    CompileOptions options;
};

std::vector<Variant> makeVariants() {
    std::vector<Variant> v;
    v.push_back({"O0", CompileOptions::o0()});
    v.push_back({"O1", CompileOptions::o1()});
    CompileOptions r3 = CompileOptions::o1();
    r3.registerLimit = 3;
    v.push_back({"O1/r3", r3});
    CompileOptions r2 = CompileOptions::o0();
    r2.registerLimit = 2;
    v.push_back({"O0/r2", r2});
    for (PassID p : allPasses()) {
        CompileOptions without = CompileOptions::o1();
        without.passes.erase(p);
        v.push_back({cat("O1-", passRawValue(p)), without});
        CompileOptions only;
        only.passes = p == PassID::Mem2Reg ? std::set<PassID>{p} : std::set<PassID>{PassID::Mem2Reg, p};
        v.push_back({cat("only-", passRawValue(p)), only});
    }
    return v;
}

const std::vector<Variant>& variants() {
    static const std::vector<Variant> v = makeVariants();
    return v;
}

std::vector<std::pair<std::string, PipelineConfig>> pipelineConfigs() {
    HierarchyConfig tiny;
    tiny.l1i = CacheConfig{"L1I", 64, 16, 1, Replacement::LRU, WritePolicy::WriteBack, 1};
    tiny.l1d = CacheConfig{"L1D", 64, 8, 2, Replacement::FIFO, WritePolicy::WriteThrough, 1};
    tiny.l2 = std::nullopt;
    tiny.dramLatency = 7;
    auto cfg = [](bool fwd, BranchStage bs, PredictorKind pk, HierarchyConfig h) {
        PipelineConfig c;
        c.forwarding = fwd;
        c.branchStage = bs;
        c.predictor = pk;
        c.hierarchy = h;
        return c;
    };
    return {
        {"fwd/EX/static", PipelineConfig()},
        {"nofwd/MEM/2bit/perfect", cfg(false, BranchStage::MEM, PredictorKind::TwoBit, HierarchyConfig::perfect())},
        {"fwd/ID/1bit/tiny", cfg(true, BranchStage::ID, PredictorKind::OneBit, tiny)},
        {"nofwd/ID/static/teaching", cfg(false, BranchStage::ID, PredictorKind::NotTaken, HierarchyConfig::teaching())},
        {"fwd/MEM/2bit/tiny", cfg(true, BranchStage::MEM, PredictorKind::TwoBit, tiny)},
    };
}

struct Failure {
    std::string description;
};

std::string describe(const ProgramOutput& o) {
    return cat("exit=", o.exitCode ? std::to_string(*o.exitCode) : std::string("nil"), " fault=", o.fault ? *o.fault : std::string("none"),
               " text=", prefixChars(debugQuoted(o.text), 400));
}

ProgramOutput runNative(const std::string& src, const std::string& tag) {
    std::string dir = tempDir() + "\\spcheck-native";
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    std::string c = dir + "\\" + tag + ".c";
    std::string bin = dir + "\\" + tag + ".exe";
    std::string out = dir + "\\" + tag + ".out";
    std::string err = dir + "\\" + tag + ".err";
    writeFile(c, prelude + src);
    std::string cc = envOr("SPCHECK_CC", "gcc");
    auto status = runProcess(cat(cc, " -std=gnu99 -fwrapv -fsigned-char -w -O1 -o ", quote(bin), " ", quote(c)), out, err, 120000);
    if (!status || *status != 0) {
        std::string e = readFile(err);
        throw Failure{cat("native compiler (", cc, ") could not compile ", tag, ":\n", e.substr(0, 1500))};
    }
    auto code = runProcess(quote(bin), out, err, 60000);
    if (!code) throw Failure{cat("native program ", tag, " did not finish")};
    ProgramOutput o;
    o.text = readFile(out);
    o.exitCode = (int32_t)*code;
    return o;
}

/// Builds and runs a program every way we know. Returns the agreed output.
ProgramOutput check(const std::string& src, const std::string& tag, bool native, bool verbose = false) {
    auto ref = Compilation::compile(src, CompileOptions::o0());
    if (!ref->program || !ref->sema || (!ref->errors().empty() && ref->failedStage)) {
        std::vector<std::string> es;
        for (auto& e : ref->errors()) es.push_back(e.description());
        throw Failure{cat(tag, ": front end rejected it: ", join(es, "; "))};
    }
    ProgramOutput expected = Interpreter::run(*ref->program, *ref->sema);
    if (expected.fault) throw Failure{cat(tag, ": interpreter fault: ", *expected.fault)};
    if (native) {
        ProgramOutput n = runNative(src, tag);
        ProgramOutput e8 = expected;
        if (e8.exitCode) e8.exitCode = *e8.exitCode & 255;
        if (n.exitCode) n.exitCode = *n.exitCode & 255;
        if (n.text != e8.text || n.exitCode != e8.exitCode) {
            throw Failure{cat(tag, ": interpreter disagrees with the native compiler\n  native: ", describe(n), "\n  interp: ", describe(e8))};
        }
    }
    for (auto& v : variants()) {
        auto c = Compilation::compile(src, v.options);
        if (!c->executable) {
            std::vector<std::string> es;
            for (auto& e : c->errors()) es.push_back(e.description());
            throw Failure{cat(tag, " [", v.name, "]: build failed at ", c->failedStage ? stageName(*c->failedStage) : "?", ": ", join(es, "; "))};
        }
        ISASimulator sim(c->executable);
        sim.run(30000000);
        if (sim.output() != expected) {
            throw Failure{cat(tag, " [", v.name, "]: ISA simulator output differs\n  expected: ", describe(expected), "\n  got:      ", describe(sim.output()))};
        }
        if (verbose) printf("  %s: ok, %d instructions\n", v.name.c_str(), sim.instret());
        if (v.name != "O0" && v.name != "O1" && v.name != "O1/r3") continue;
        // The pipeline must reach exactly the same architectural result.
        for (auto& [pname, pc] : pipelineConfigs()) {
            Pipeline p(c->executable, pc);
            p.recordHistory = false;
            p.run(200000000);
            if (p.output() != expected || p.stats().retired != sim.instret()) {
                throw Failure{cat(tag, " [", v.name, ", pipeline ", pname, "]: differs from the ISA model\n  expected: ", describe(expected),
                                  " instret=", sim.instret(), "\n  got:      ", describe(p.output()), " retired=", p.stats().retired)};
            }
            if (verbose) printf("    pipeline %s: %d cycles, CPI %.2f\n", pname.c_str(), p.stats().cycles, p.stats().cpi());
        }
    }
    return expected;
}

/// Compiles and runs every built-in example on both CPU models and checks them against the interpreter.
bool verifyExamples() {
    bool ok = true;
    for (auto& ex : Examples::all()) {
        auto c = ex.isAssembly ? Compilation::assemble(ex.source) : Compilation::compile(ex.source, CompileOptions::o1());
        if (!c->executable) {
            std::vector<std::string> es;
            for (auto& e : c->errors()) es.push_back(e.description());
            printf("FAIL %s: %s\n", ex.name.c_str(), join(es, "; ").c_str());
            ok = false;
            continue;
        }
        ISASimulator isa(c->executable);
        isa.run();
        Pipeline p(c->executable);
        p.recordHistory = false;
        p.run();
        bool verdict = isa.output() == p.output() && isa.instret() == p.stats().retired && !isa.output().fault;
        if (c->program && c->sema) {
            ProgramOutput ref = Interpreter::run(*c->program, *c->sema);
            verdict = verdict && ref == isa.output();
        }
        if (!ex.isAssembly) {
            CompileOptions r3 = CompileOptions::o1();
            r3.registerLimit = 3;
            for (const CompileOptions& opt : {CompileOptions::o0(), r3}) {
                auto c2 = Compilation::compile(ex.source, opt);
                if (c2->executable) {
                    ISASimulator s2(c2->executable);
                    s2.run();
                    verdict = verdict && s2.output() == isa.output();
                } else {
                    verdict = false;
                }
            }
        }
        ok = ok && verdict;
        std::vector<std::string> warn;
        for (auto& d : c->diagnostics)
            if (d.severity == Diagnostic::Severity::Warning) warn.push_back(d.description());
        printf("%s %s: %d instructions, %d cycles, CPI %.2f \xc2\xb7 output %s%s\n", verdict ? "ok  " : "FAIL", ex.name.c_str(), isa.instret(),
               p.stats().cycles, p.stats().cpi(), debugQuoted(prefixChars(isa.output().text, 60)).c_str(),
               warn.empty() ? "" : (" \xc2\xb7 warnings: " + join(warn, "; ")).c_str());
    }
    return ok;
}

std::string firstLine(const std::string& s) {
    size_t k = s.find('\n');
    return k == std::string::npos ? s : s.substr(0, k);
}

}  // namespace

int wmain(int argc, wchar_t** wargv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, nullptr, _IOFBF, 1 << 16);
    std::vector<std::string> args;
    for (int i = 0; i < argc; i++) args.push_back(narrow(wargv[i]));
    std::string mode = args.size() > 1 ? args[1] : "hand";
    try {
        if (mode == "hand") {
            int failed = 0;
            for (auto& [name, src] : spcheck::handTests()) {
                try {
                    ProgramOutput out = check(src, name, true);
                    printf("PASS %s: %s\n", name.c_str(), firstLine(out.text).c_str());
                } catch (const Failure& f) {
                    failed += 1;
                    printf("FAIL %s\n", f.description.c_str());
                }
                fflush(stdout);
            }
            int total = (int)spcheck::handTests().size();
            printf("%d/%d hand tests passed\n", total - failed, total);
            return failed == 0 ? 0 : 1;
        }
        if (mode == "fuzz" || mode == "native" || mode == "clang") {
            int n = args.size() > 2 ? (int)parseInt(args[2]).value_or(100) : 100;
            uint64_t seed = args.size() > 3 ? (uint64_t)parseInt(args[3]).value_or(1) : 1;
            bool native = mode != "fuzz";
            int failed = 0;
            auto t0 = std::chrono::steady_clock::now();
            int k = 0;
            for (; k < n; k++) {
                uint64_t s = seed + (uint64_t)k;
                std::string src = spcheck::generateProgram(s);
                try {
                    check(src, cat("seed", s), native);
                } catch (const Failure& f) {
                    failed += 1;
                    printf("FAIL seed %llu: %s\n", (unsigned long long)s, f.description.c_str());
                    if (failed >= 5) break;
                }
                if ((k + 1) % 25 == 0) {
                    int secs = (int)std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                    printf("\xe2\x80\xa6 %d programs, %d failures, %ds\n", k + 1, failed, secs);
                    fflush(stdout);
                }
            }
            printf("%d/%d random programs agree across %d build variants%s\n", n - failed, n, (int)variants().size(),
                   native ? " and the native compiler" : "");
            return failed == 0 ? 0 : 1;
        }
        if (mode == "show") {
            uint64_t s = args.size() > 2 ? (uint64_t)parseInt(args[2]).value_or(1) : 1;
            printf("%s", spcheck::generateProgram(s).c_str());
            return 0;
        }
        if (mode == "file") {
            std::string src = replaceAll(readFile(args.at(2)), "\r\n", "\n");
            try {
                ProgramOutput out = check(src, "file", true, true);
                printf("%s\n", out.text.c_str());
            } catch (const Failure& f) {
                printf("FAIL %s\n", f.description.c_str());
                return 1;
            }
            return 0;
        }
        if (mode == "asm") {
            std::string src = replaceAll(readFile(args.at(2)), "\r\n", "\n");
            auto c = Compilation::compile(src, args.size() > 3 && args[3] == "O0" ? CompileOptions::o0() : CompileOptions::o1());
            for (auto& d : c->diagnostics) printf("%s\n", d.description().c_str());
            for (auto& l : c->asmLines) printf("%s%s\n", l.text.c_str(), l.note.empty() ? "" : ("    # " + l.note).c_str());
            return 0;
        }
        if (mode == "verify") return verifyExamples() ? 0 : 1;
    } catch (const std::exception& e) {
        printf("error: %s\n", e.what());
        return 2;
    }
    printf("usage: spcheck hand | fuzz N [seed] | native N [seed] | show seed | file x.c | asm x.c [O0] | verify\n");
    return 2;
}
