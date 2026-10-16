import Foundation

/// Generates random, well-defined C programs (no undefined behaviour) that
/// print a trace of their values. Used for differential testing: the same
/// program must print the same thing under the reference interpreter, every
/// optimisation setting, both CPU models, and the Mac's own clang.
struct RNG {
    var state: UInt64
    init(_ seed: UInt64) { state = seed &* 6364136223846793005 &+ 1442695040888963407 }
    mutating func next() -> UInt64 {
        state = state &* 6364136223846793005 &+ 1442695040888963407
        var x = state
        x ^= x >> 33
        x = x &* 0xff51afd7ed558ccd
        x ^= x >> 33
        return x
    }
    mutating func int(_ n: Int) -> Int { Int(next() % UInt64(max(1, n))) }
    mutating func chance(_ p: Double) -> Bool { Double(next() % 10000) / 10000 < p }
    mutating func pick<T>(_ a: [T]) -> T { a[int(a.count)] }
}

final class ProgramGenerator {
    var rng: RNG
    var out: [String] = []
    var indent = 1
    var ints: [String] = []      // int variables in scope
    var uints: [String] = []
    var chars: [String] = []
    var arrays: [String] = []    // int arrays of 8
    var loopVars: Set<String> = []
    var depth = 0
    var counter = 0
    var functions: [(name: String, params: Int)] = []
    let globalsInt = ["g0", "g1", "g2"]

    init(seed: UInt64) { rng = RNG(seed) }

    func fresh(_ p: String) -> String {
        counter += 1
        return "\(p)\(counter)"
    }

    func line(_ s: String) { out.append(String(repeating: "    ", count: indent) + s) }

    func constant() -> String {
        switch rng.int(10) {
        case 0: return String(rng.pick([2147483647, -2147483647, 65535, -65536, 100000, 0x7FFF, 0x12345678]))
        case 1: return "0x" + String(rng.int(1 << 16), radix: 16)
        case 2: return "'\(rng.pick(["a", "Z", "0", "~"]))'"
        default: return String(rng.int(41) - 20)
        }
    }

    /// A pure expression of type int (no side effects, no undefined behaviour).
    func expr(_ d: Int = 0) -> String {
        if d > 3 || rng.chance(0.3) {
            var leaves = ints + globalsInt + [constant()]
            if !chars.isEmpty { leaves += chars }
            if !uints.isEmpty && rng.chance(0.2) { return "(int)" + rng.pick(uints) }
            if !arrays.isEmpty && rng.chance(0.25) { return "\(rng.pick(arrays))[(\(expr(d + 2))) & 7]" }
            if rng.chance(0.1) { return "gs.a" }
            if rng.chance(0.05) { return "garr[(\(expr(d + 2))) & 15]" }
            return rng.pick(leaves)
        }
        let a = expr(d + 1), b = expr(d + 1)
        switch rng.int(22) {
        case 0, 1: return "(\(a) + \(b))"
        case 2, 3: return "(\(a) - \(b))"
        case 4: return "(\(a) * \(b))"
        case 5: return "(\(a) / (((\(b)) & 15) + 1))"
        case 6: return "(\(a) % (((\(b)) & 15) + 1))"
        case 7: return "(\(a) & \(b))"
        case 8: return "(\(a) | \(b))"
        case 9: return "(\(a) ^ \(b))"
        case 10: return "(int)((unsigned)(\(a)) << ((\(b)) & 31))"
        case 11: return "(\(a) >> ((\(b)) & 31))"
        case 12: return "(int)((unsigned)(\(a)) >> ((\(b)) & 31))"
        case 13: return "(\(a) \(rng.pick(["<", ">", "<=", ">=", "==", "!="])) \(b))"
        case 14: return "(\(a) \(rng.pick(["&&", "||"])) \(b))"
        case 15: return "(\(rng.pick(["-", "~", "!"])) \(a))"
        case 16: return "(\(expr(d + 1)) ? \(a) : \(b))"
        case 17: return "(char)(\(a))"
        case 18: return "((unsigned)(\(a)) \(rng.pick(["<", ">", "<=", ">="])) (unsigned)(\(b)))"
        case 19: return "(int)((unsigned)(\(a)) / ((unsigned)(\(b)) % 7u + 1u))"
        case 20: return "(\(a) * \(rng.pick(["2", "4", "8", "16", "3", "-1", "0", "1"])))"
        default: return "(\(a) / \(rng.pick(["2", "4", "8", "16", "1", "-4"])))"
        }
    }

    func assignable() -> String? {
        let candidates = ints.filter { !loopVars.contains($0) } + globalsInt
        if candidates.isEmpty { return nil }
        return rng.pick(candidates)
    }

    func statement() {
        guard depth < 3 else {
            simpleStatement()
            return
        }
        switch rng.int(16) {
        case 0, 1, 2: simpleStatement()
        case 3:
            line("if (\(expr())) {")
            block()
            if rng.chance(0.5) {
                line("} else {")
                block()
            }
            line("}")
        case 4:
            let i = fresh("i")
            let n = rng.int(6) + 1
            line("for (int \(i) = 0; \(i) < \(n); \(i)++) {")
            loopVars.insert(i)
            ints.append(i)
            block(breakOK: true)
            ints.removeLast()
            loopVars.remove(i)
            line("}")
        case 5:
            let w = fresh("w")
            line("int \(w) = \(rng.int(5) + 1);")
            line("while (\(w) > 0) {")
            loopVars.insert(w)
            ints.append(w)
            block(breakOK: true, continueOK: false)
            line("\(w)--;")
            ints.removeLast()
            loopVars.remove(w)
            line("}")
        case 6:
            let w = fresh("d")
            line("int \(w) = \(rng.int(4));")
            line("do {")
            loopVars.insert(w)
            ints.append(w)
            block()
            line("\(w)--;")
            ints.removeLast()
            loopVars.remove(w)
            line("} while (\(w) > 0);")
        case 7:
            line("switch ((\(expr())) & 3) {")
            for k in 0..<4 where rng.chance(0.7) {
                line("case \(k):")
                indent += 1
                simpleStatement()
                if rng.chance(0.7) { line("break;") }
                indent -= 1
            }
            if rng.chance(0.5) {
                line("default:")
                indent += 1
                simpleStatement()
                indent -= 1
            }
            line("}")
        case 8:
            if let f = functions.isEmpty ? nil : rng.pick(functions), let v = assignable() {
                let args = (0..<f.params).map { _ in expr(2) }.joined(separator: ", ")
                line("\(v) = \(f.name)(\(args));")
            } else {
                simpleStatement()
            }
        case 9:
            if !arrays.isEmpty, let v = assignable() {
                line("\(v) = sumarr(\(rng.pick(arrays)), \(rng.int(8) + 1));")
            } else { simpleStatement() }
        case 10:
            let p = fresh("p")
            if let v = ints.filter({ !loopVars.contains($0) }).randomElement(using: &rng) {
                line("int *\(p) = &\(v);")
                line("*\(p) \(rng.pick(["=", "+=", "^=", "-="])) \(expr(1));")
            } else if !arrays.isEmpty {
                line("int *\(p) = \(rng.pick(arrays)) + ((\(expr(2))) & 7);")
                line("*\(p) = \(expr(1));")
            }
        case 11:
            line("gs.\(rng.pick(["a", "b"])) = \(expr(1));")
            line("gs.c = (char)(\(expr(2)));")
            line("bump(&gs, \(expr(2)));")
        default:
            simpleStatement()
        }
    }

    func simpleStatement() {
        switch rng.int(12) {
        case 0, 1, 2, 3:
            if let v = assignable() {
                let op = rng.pick(["=", "=", "+=", "-=", "*=", "^=", "|=", "&="])
                line("\(v) \(op) \(expr());")
            }
        case 4:
            line("printf(\"%d\\n\", \(expr()));")
        case 5:
            if !arrays.isEmpty { line("\(rng.pick(arrays))[(\(expr(2))) & 7] = \(expr(1));") }
        case 6:
            if let v = assignable() { line(rng.chance(0.5) ? "\(v)++;" : "--\(v);") }
        case 7:
            if !chars.isEmpty { line("\(rng.pick(chars)) = (char)(\(expr(1)));") }
        case 8:
            if !uints.isEmpty {
                let u = rng.pick(uints)
                line("\(u) = \(u) * 2654435761u + (unsigned)(\(expr(2)));")
                line("printf(\"%u %x\\n\", \(u), \(u) >> 7);")
            }
        case 9:
            line("garr[(\(expr(2))) & 15] += \(expr(2));")
        case 10:
            if let v = assignable() { line("\(v) = rec(((\(expr(2))) & 7));") }
        default:
            if let v = ints.randomElement(using: &rng) { line("printf(\"%d %c\\n\", \(v), 'a' + ((\(v)) & 15));") }
        }
    }

    func block(breakOK: Bool = false, continueOK: Bool = true) {
        indent += 1
        depth += 1
        let saved = (ints.count, chars.count, arrays.count, uints.count)
        if rng.chance(0.3) {
            let v = fresh("v")
            line("int \(v) = \(expr());")
            ints.append(v)
        }
        for _ in 0..<(rng.int(3) + 1) { statement() }
        if breakOK && rng.chance(0.15) { line("if (\(expr(2))) \(continueOK ? rng.pick(["break", "continue"]) : "break");") }
        ints.removeLast(ints.count - saved.0)
        chars.removeLast(chars.count - saved.1)
        arrays.removeLast(arrays.count - saved.2)
        uints.removeLast(uints.count - saved.3)
        depth -= 1
        indent -= 1
    }

    func function(_ name: String, params: Int) -> [String] {
        out = []
        indent = 1
        ints = (0..<params).map { "a\($0)" }
        chars = []
        arrays = []
        uints = []
        let sig = (0..<params).map { "int a\($0)" }.joined(separator: ", ")
        var body: [String] = []
        let r = fresh("r")
        line("int \(r) = \(expr());")
        ints.append(r)
        for _ in 0..<(rng.int(3) + 1) { statement() }
        line("return \(r) + \(expr(2));")
        body = out
        return ["int \(name)(\(sig)) {"] + body + ["}"]
    }

    func generate() -> String {
        var lines: [String] = [
            "struct S { int a; char c; int b; };",
            "int g0 = \(rng.int(100)), g1 = \(rng.int(200) - 100), g2 = \(constant());",
            "int garr[16] = {\((0..<rng.int(16)).map { _ in String(rng.int(50) - 25) }.joined(separator: ", "))};",
            "struct S gs = {\(rng.int(9)), 'q', \(rng.int(9))};",
            "int rec(int n) { if (n <= 0) return 1; return rec(n - 1) * 3 + n; }",
            "int sumarr(int *p, int n) { int s = 0; for (int i = 0; i < n; i++) s += p[i]; return s; }",
            "void bump(struct S *s, int k) { s->a += k; s->b ^= s->a; s->c = (char)(s->c + 1); }",
        ]
        for k in 0..<(rng.int(3) + 1) {
            let name = "f\(k)"
            let params = rng.int(4)
            lines += function(name, params: params)
            functions.append((name, params))
        }
        out = []
        indent = 1
        ints = []
        chars = []
        arrays = []
        uints = []
        for _ in 0..<(rng.int(4) + 2) {
            let v = fresh("x")
            line("int \(v) = \(expr());")
            ints.append(v)
        }
        let c = fresh("c")
        line("char \(c) = (char)(\(expr()));")
        chars.append(c)
        let u = fresh("u")
        line("unsigned \(u) = (unsigned)(\(expr())) + 7u;")
        uints.append(u)
        let a = fresh("arr")
        line("int \(a)[8] = {\((0..<rng.int(9)).map { _ in String(rng.int(30) - 10) }.joined(separator: ", "))};")
        arrays.append(a)
        for _ in 0..<(rng.int(8) + 4) { statement() }
        for v in ints { line("printf(\"\(v)=%d\\n\", \(v));") }
        line("printf(\"\(c)=%d \(u)=%u\\n\", \(c), \(u));")
        line("for (int k = 0; k < 8; k++) printf(\"%d \", \(a)[k]);")
        line("printf(\"\\ng=%d %d %d s=%d %d %d\\n\", g0, g1, g2, gs.a, gs.b, gs.c);")
        line("for (int k = 0; k < 16; k++) printf(\"%d,\", garr[k]);")
        line("return (g0 ^ g1) & 255;")
        lines += ["int main() {"] + out + ["}"]
        return lines.joined(separator: "\n") + "\n"
    }
}

extension Array {
    func randomElement(using rng: inout RNG) -> Element? {
        isEmpty ? nil : self[rng.int(count)]
    }
}
