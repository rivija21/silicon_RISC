#include <set>

#include "../core/Base.h"
#include "Tests.h"

namespace spcheck {

using sp::cat;

namespace {

struct RNG {
    uint64_t state;
    explicit RNG(uint64_t seed) { state = seed * 6364136223846793005ull + 1442695040888963407ull; }
    uint64_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        uint64_t x = state;
        x ^= x >> 33;
        x = x * 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        return x;
    }
    int integer(int n) { return (int)(next() % (uint64_t)std::max(1, n)); }
    bool chance(double p) { return (double)(next() % 10000) / 10000 < p; }
    template <class T>
    T pick(const std::vector<T>& a) { return a[integer((int)a.size())]; }
};

class ProgramGenerator {
public:
    explicit ProgramGenerator(uint64_t seed) : rng(seed) {}

    std::string generate() {
        std::vector<std::string> lines;
        lines.push_back("struct S { int a; char c; int b; };");
        {
            int g0 = rng.integer(100);
            int g1 = rng.integer(200) - 100;
            std::string g2 = constant();
            lines.push_back(cat("int g0 = ", g0, ", g1 = ", g1, ", g2 = ", g2, ";"));
        }
        {
            int n = rng.integer(16);
            std::vector<std::string> vals;
            for (int k = 0; k < n; k++) vals.push_back(std::to_string(rng.integer(50) - 25));
            lines.push_back("int garr[16] = {" + sp::join(vals, ", ") + "};");
        }
        {
            int a = rng.integer(9);
            int b = rng.integer(9);
            lines.push_back(cat("struct S gs = {", a, ", 'q', ", b, "};"));
        }
        lines.push_back("int rec(int n) { if (n <= 0) return 1; return rec(n - 1) * 3 + n; }");
        lines.push_back("int sumarr(int *p, int n) { int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }");
        lines.push_back("void bump(struct S *s, int k) { s->a += k; s->b ^= s->a; s->c = (char)(s->c + 1); }");
        int nf = rng.integer(3) + 1;
        for (int k = 0; k < nf; k++) {
            std::string name = cat("f", k);
            int params = rng.integer(4);
            auto fl = function(name, params);
            lines.insert(lines.end(), fl.begin(), fl.end());
            functions.push_back({name, params});
        }
        out.clear();
        indent = 1;
        ints.clear();
        chars.clear();
        arrays.clear();
        uints.clear();
        int nx = rng.integer(4) + 2;
        for (int k = 0; k < nx; k++) {
            std::string v = fresh("x");
            line(cat("int ", v, " = ", expr(), ";"));
            ints.push_back(v);
        }
        std::string c = fresh("c");
        line(cat("char ", c, " = (char)(", expr(), ");"));
        chars.push_back(c);
        std::string u = fresh("u");
        line(cat("unsigned ", u, " = (unsigned)(", expr(), ") + 7u;"));
        uints.push_back(u);
        std::string a = fresh("arr");
        {
            int n = rng.integer(9);
            std::vector<std::string> vals;
            for (int k = 0; k < n; k++) vals.push_back(std::to_string(rng.integer(30) - 10));
            line(cat("int ", a, "[8] = {", sp::join(vals, ", "), "};"));
        }
        arrays.push_back(a);
        int ns = rng.integer(8) + 4;
        for (int k = 0; k < ns; k++) statement();
        for (auto& v : ints) line(cat("printf(\"", v, "=%d\\n\", ", v, ");"));
        line(cat("printf(\"", c, "=%d ", u, "=%u\\n\", ", c, ", ", u, ");"));
        line(cat("for (int k = 0; k < 8; k++) printf(\"%d \", ", a, "[k]);"));
        line("printf(\"\\ng=%d %d %d s=%d %d %d\\n\", g0, g1, g2, gs.a, gs.b, gs.c);");
        line("for (int k = 0; k < 16; k++) printf(\"%d,\", garr[k]);");
        line("return (g0 ^ g1) & 255;");
        lines.push_back("int main() {");
        lines.insert(lines.end(), out.begin(), out.end());
        lines.push_back("}");
        return sp::join(lines, "\n") + "\n";
    }

private:
    RNG rng;
    std::vector<std::string> out;
    int indent = 1;
    std::vector<std::string> ints, uints, chars, arrays;
    std::set<std::string> loopVars;
    int depth = 0;
    int counter = 0;
    std::vector<std::pair<std::string, int>> functions;
    const std::vector<std::string> globalsInt = {"g0", "g1", "g2"};

    std::string fresh(const std::string& p) {
        counter += 1;
        return cat(p, counter);
    }

    void line(const std::string& s) { out.push_back(sp::repeated("    ", indent) + s); }

    std::string constant() {
        switch (rng.integer(10)) {
        case 0: return std::to_string(rng.pick(std::vector<int>{2147483647, -2147483647, 65535, -65536, 100000, 0x7FFF, 0x12345678}));
        case 1: return "0x" + sp::hexString((uint64_t)rng.integer(1 << 16));
        case 2: return "'" + rng.pick(std::vector<std::string>{"a", "Z", "0", "~"}) + "'";
        default: return std::to_string(rng.integer(41) - 20);
        }
    }

    /// A pure expression of type int (no side effects, no undefined behaviour).
    std::string expr(int d = 0) {
        if (d > 3 || rng.chance(0.3)) {
            std::vector<std::string> leaves = ints;
            leaves.insert(leaves.end(), globalsInt.begin(), globalsInt.end());
            leaves.push_back(constant());
            if (!chars.empty()) leaves.insert(leaves.end(), chars.begin(), chars.end());
            if (!uints.empty() && rng.chance(0.2)) return "(int)" + rng.pick(uints);
            if (!arrays.empty() && rng.chance(0.25)) {
                std::string arr = rng.pick(arrays);
                return cat(arr, "[(", expr(d + 2), ") & 7]");
            }
            if (rng.chance(0.1)) return "gs.a";
            if (rng.chance(0.05)) return cat("garr[(", expr(d + 2), ") & 15]");
            return rng.pick(leaves);
        }
        std::string a = expr(d + 1);
        std::string b = expr(d + 1);
        switch (rng.integer(22)) {
        case 0: case 1: return cat("(", a, " + ", b, ")");
        case 2: case 3: return cat("(", a, " - ", b, ")");
        case 4: return cat("(", a, " * ", b, ")");
        case 5: return cat("(", a, " / (((", b, ") & 15) + 1))");
        case 6: return cat("(", a, " % (((", b, ") & 15) + 1))");
        case 7: return cat("(", a, " & ", b, ")");
        case 8: return cat("(", a, " | ", b, ")");
        case 9: return cat("(", a, " ^ ", b, ")");
        case 10: return cat("(int)((unsigned)(", a, ") << ((", b, ") & 31))");
        case 11: return cat("(", a, " >> ((", b, ") & 31))");
        case 12: return cat("(int)((unsigned)(", a, ") >> ((", b, ") & 31))");
        case 13: {
            std::string op = rng.pick(std::vector<std::string>{"<", ">", "<=", ">=", "==", "!="});
            return cat("(", a, " ", op, " ", b, ")");
        }
        case 14: {
            std::string op = rng.pick(std::vector<std::string>{"&&", "||"});
            return cat("(", a, " ", op, " ", b, ")");
        }
        case 15: {
            std::string op = rng.pick(std::vector<std::string>{"-", "~", "!"});
            return cat("(", op, " ", a, ")");
        }
        case 16: {
            std::string c = expr(d + 1);
            return cat("(", c, " ? ", a, " : ", b, ")");
        }
        case 17: return cat("(char)(", a, ")");
        case 18: {
            std::string op = rng.pick(std::vector<std::string>{"<", ">", "<=", ">="});
            return cat("((unsigned)(", a, ") ", op, " (unsigned)(", b, "))");
        }
        case 19: return cat("(int)((unsigned)(", a, ") / ((unsigned)(", b, ") % 7u + 1u))");
        case 20: {
            std::string k = rng.pick(std::vector<std::string>{"2", "4", "8", "16", "3", "-1", "0", "1"});
            return cat("(", a, " * ", k, ")");
        }
        default: {
            std::string k = rng.pick(std::vector<std::string>{"2", "4", "8", "16", "1", "-4"});
            return cat("(", a, " / ", k, ")");
        }
        }
    }

    std::vector<std::string> nonLoopInts() {
        std::vector<std::string> c;
        for (auto& v : ints)
            if (!loopVars.count(v)) c.push_back(v);
        return c;
    }

    std::optional<std::string> assignable() {
        std::vector<std::string> candidates = nonLoopInts();
        candidates.insert(candidates.end(), globalsInt.begin(), globalsInt.end());
        if (candidates.empty()) return std::nullopt;
        return rng.pick(candidates);
    }

    std::optional<std::string> randomElement(const std::vector<std::string>& v) {
        if (v.empty()) return std::nullopt;
        return v[rng.integer((int)v.size())];
    }

    void statement() {
        if (depth >= 3) {
            simpleStatement();
            return;
        }
        switch (rng.integer(16)) {
        case 0: case 1: case 2: simpleStatement(); break;
        case 3:
            line(cat("if (", expr(), ") {"));
            block();
            if (rng.chance(0.5)) {
                line("} else {");
                block();
            }
            line("}");
            break;
        case 4: {
            std::string i = fresh("i");
            int n = rng.integer(6) + 1;
            line(cat("for (int ", i, " = 0; ", i, " < ", n, "; ", i, "++) {"));
            loopVars.insert(i);
            ints.push_back(i);
            block(true);
            ints.pop_back();
            loopVars.erase(i);
            line("}");
            break;
        }
        case 5: {
            std::string w = fresh("w");
            line(cat("int ", w, " = ", rng.integer(5) + 1, ";"));
            line(cat("while (", w, " > 0) {"));
            loopVars.insert(w);
            ints.push_back(w);
            block(true, false);
            line(cat(w, "--;"));
            ints.pop_back();
            loopVars.erase(w);
            line("}");
            break;
        }
        case 6: {
            std::string w = fresh("d");
            line(cat("int ", w, " = ", rng.integer(4), ";"));
            line("do {");
            loopVars.insert(w);
            ints.push_back(w);
            block();
            line(cat(w, "--;"));
            ints.pop_back();
            loopVars.erase(w);
            line(cat("} while (", w, " > 0);"));
            break;
        }
        case 7:
            line(cat("switch ((", expr(), ") & 3) {"));
            for (int k = 0; k < 4; k++) {
                if (!rng.chance(0.7)) continue;
                line(cat("case ", k, ":"));
                indent += 1;
                simpleStatement();
                if (rng.chance(0.7)) line("break;");
                indent -= 1;
            }
            if (rng.chance(0.5)) {
                line("default:");
                indent += 1;
                simpleStatement();
                indent -= 1;
            }
            line("}");
            break;
        case 8: {
            std::optional<std::pair<std::string, int>> f;
            if (!functions.empty()) f = rng.pick(functions);
            std::optional<std::string> v;
            if (f) v = assignable();
            if (f && v) {
                std::vector<std::string> args;
                for (int k = 0; k < f->second; k++) args.push_back(expr(2));
                line(cat(*v, " = ", f->first, "(", sp::join(args, ", "), ");"));
            } else {
                simpleStatement();
            }
            break;
        }
        case 9: {
            std::optional<std::string> v;
            if (!arrays.empty()) v = assignable();
            if (!arrays.empty() && v) {
                std::string arr = rng.pick(arrays);
                line(cat(*v, " = sumarr(", arr, ", ", rng.integer(8) + 1, ");"));
            } else {
                simpleStatement();
            }
            break;
        }
        case 10: {
            std::string p = fresh("p");
            if (auto v = randomElement(nonLoopInts())) {
                line(cat("int *", p, " = &", *v, ";"));
                std::string op = rng.pick(std::vector<std::string>{"=", "+=", "^=", "-="});
                line(cat("*", p, " ", op, " ", expr(1), ";"));
            } else if (!arrays.empty()) {
                std::string arr = rng.pick(arrays);
                line(cat("int *", p, " = ", arr, " + ((", expr(2), ") & 7);"));
                line(cat("*", p, " = ", expr(1), ";"));
            }
            break;
        }
        case 11: {
            std::string field = rng.pick(std::vector<std::string>{"a", "b"});
            line(cat("gs.", field, " = ", expr(1), ";"));
            line(cat("gs.c = (char)(", expr(2), ");"));
            line(cat("bump(&gs, ", expr(2), ");"));
            break;
        }
        default: simpleStatement(); break;
        }
    }

    void simpleStatement() {
        switch (rng.integer(12)) {
        case 0: case 1: case 2: case 3:
            if (auto v = assignable()) {
                std::string op = rng.pick(std::vector<std::string>{"=", "=", "+=", "-=", "*=", "^=", "|=", "&="});
                line(cat(*v, " ", op, " ", expr(), ";"));
            }
            break;
        case 4: line(cat("printf(\"%d\\n\", ", expr(), ");")); break;
        case 5:
            if (!arrays.empty()) {
                std::string arr = rng.pick(arrays);
                std::string i = expr(2);
                line(cat(arr, "[(", i, ") & 7] = ", expr(1), ";"));
            }
            break;
        case 6:
            if (auto v = assignable()) line(rng.chance(0.5) ? cat(*v, "++;") : cat("--", *v, ";"));
            break;
        case 7:
            if (!chars.empty()) {
                std::string c = rng.pick(chars);
                line(cat(c, " = (char)(", expr(1), ");"));
            }
            break;
        case 8:
            if (!uints.empty()) {
                std::string u = rng.pick(uints);
                line(cat(u, " = ", u, " * 2654435761u + (unsigned)(", expr(2), ");"));
                line(cat("printf(\"%u %x\\n\", ", u, ", ", u, " >> 7);"));
            }
            break;
        case 9: {
            std::string i = expr(2);
            line(cat("garr[(", i, ") & 15] += ", expr(2), ";"));
            break;
        }
        case 10:
            if (auto v = assignable()) line(cat(*v, " = rec(((", expr(2), ") & 7));"));
            break;
        default:
            if (auto v = randomElement(ints)) line(cat("printf(\"%d %c\\n\", ", *v, ", 'a' + ((", *v, ") & 15));"));
            break;
        }
    }

    void block(bool breakOK = false, bool continueOK = true) {
        indent += 1;
        depth += 1;
        size_t s0 = ints.size(), s1 = chars.size(), s2 = arrays.size(), s3 = uints.size();
        if (rng.chance(0.3)) {
            std::string v = fresh("v");
            line(cat("int ", v, " = ", expr(), ";"));
            ints.push_back(v);
        }
        int n = rng.integer(3) + 1;
        for (int k = 0; k < n; k++) statement();
        if (breakOK && rng.chance(0.15)) {
            std::string c = expr(2);
            std::string kw = continueOK ? rng.pick(std::vector<std::string>{"break", "continue"}) : "break";
            line(cat("if (", c, ") ", kw, ";"));
        }
        ints.resize(s0);
        chars.resize(s1);
        arrays.resize(s2);
        uints.resize(s3);
        depth -= 1;
        indent -= 1;
    }

    std::vector<std::string> function(const std::string& name, int params) {
        out.clear();
        indent = 1;
        ints.clear();
        for (int k = 0; k < params; k++) ints.push_back(cat("a", k));
        chars.clear();
        arrays.clear();
        uints.clear();
        std::vector<std::string> sig;
        for (int k = 0; k < params; k++) sig.push_back(cat("int a", k));
        std::string r = fresh("r");
        line(cat("int ", r, " = ", expr(), ";"));
        ints.push_back(r);
        int n = rng.integer(3) + 1;
        for (int k = 0; k < n; k++) statement();
        line(cat("return ", r, " + ", expr(2), ";"));
        std::vector<std::string> result{cat("int ", name, "(", sp::join(sig, ", "), ") {")};
        result.insert(result.end(), out.begin(), out.end());
        result.push_back("}");
        return result;
    }
};

}  // namespace

std::string generateProgram(uint64_t seed) {
    ProgramGenerator g(seed);
    return g.generate();
}

}  // namespace spcheck
