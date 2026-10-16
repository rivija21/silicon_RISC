import Foundation
import SiliconCore

// spcheck — the compiler's test driver.
//
//   spcheck hand            hand-written programs, checked against native clang
//   spcheck fuzz N [seed]   N random programs, differential testing
//   spcheck clang N [seed]  random programs also checked against native clang
//   spcheck show seed       print one generated program
//   spcheck file x.c        compile and run one file in every configuration

let scratch = ProcessInfo.processInfo.environment["SPCHECK_TMP"] ?? NSTemporaryDirectory()

let prelude = """
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

"""

/// Every way of building and running a program that must agree.
struct Variant {
    var name: String
    var options: CompileOptions
}

var variants: [Variant] = [
    Variant(name: "O0", options: .o0),
    Variant(name: "O1", options: .o1),
    Variant(name: "O1/r3", options: CompileOptions(registerLimit: 3)),
    Variant(name: "O0/r2", options: CompileOptions(passes: [], registerLimit: 2)),
]
for p in PassID.allCases {
    variants.append(Variant(name: "O1-\(p.rawValue)", options: CompileOptions(passes: Set(PassID.allCases).subtracting([p]))))
    variants.append(Variant(name: "only-\(p.rawValue)", options: CompileOptions(passes: p == .mem2reg ? [p] : [.mem2reg, p])))
}

let tinyCaches = HierarchyConfig(
    l1i: CacheConfig(name: "L1I", size: 64, blockSize: 16, ways: 1, latency: 1),
    l1d: CacheConfig(name: "L1D", size: 64, blockSize: 8, ways: 2, replacement: .fifo, write: .writeThrough, latency: 1),
    l2: nil, dramLatency: 7)
let pipelineConfigs: [(String, PipelineConfig)] = [
    ("fwd/EX/static", PipelineConfig()),
    ("nofwd/MEM/2bit/perfect", PipelineConfig(forwarding: false, branchStage: .mem, predictor: .twoBit, hierarchy: .perfect)),
    ("fwd/ID/1bit/tiny", PipelineConfig(forwarding: true, branchStage: .id, predictor: .oneBit, hierarchy: tinyCaches)),
    ("nofwd/ID/static/teaching", PipelineConfig(forwarding: false, branchStage: .id, predictor: .notTaken)),
    ("fwd/MEM/2bit/tiny", PipelineConfig(forwarding: true, branchStage: .mem, predictor: .twoBit, hierarchy: tinyCaches)),
]

struct Failure: Error, CustomStringConvertible {
    var description: String
}

func describe(_ o: ProgramOutput) -> String {
    "exit=\(o.exitCode.map(String.init) ?? "nil") fault=\(o.fault ?? "none") text=\(o.text.debugDescription.prefix(400))"
}

func runNative(_ src: String, tag: String) throws -> ProgramOutput {
    let dir = (scratch as NSString).appendingPathComponent("spcheck-native")
    try FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
    let c = (dir as NSString).appendingPathComponent("\(tag).c")
    let bin = (dir as NSString).appendingPathComponent("\(tag).bin")
    try (prelude + src).write(toFile: c, atomically: true, encoding: .utf8)
    let cc = Process()
    cc.executableURL = URL(fileURLWithPath: "/usr/bin/clang")
    cc.arguments = ["-std=gnu99", "-fwrapv", "-fsigned-char", "-w", "-O1", "-o", bin, c]
    let errPipe = Pipe()
    cc.standardError = errPipe
    try cc.run()
    cc.waitUntilExit()
    if cc.terminationStatus != 0 {
        let e = String(decoding: errPipe.fileHandleForReading.readDataToEndOfFile(), as: UTF8.self)
        throw Failure(description: "native clang could not compile \(tag):\n\(e.prefix(1500))")
    }
    let run = Process()
    run.executableURL = URL(fileURLWithPath: bin)
    let outPipe = Pipe()
    run.standardOutput = outPipe
    try run.run()
    let data = outPipe.fileHandleForReading.readDataToEndOfFile()
    run.waitUntilExit()
    var o = ProgramOutput()
    o.text = String(decoding: data, as: UTF8.self)
    o.exitCode = run.terminationStatus
    return o
}

/// Builds and runs a program every way we know. Returns the agreed output.
@discardableResult
func check(_ src: String, tag: String, native: Bool, verbose: Bool = false) throws -> ProgramOutput {
    let ref = Compilation.compile(src, options: .o0)
    guard let prog = ref.program, let sema = ref.sema, ref.errors.isEmpty || ref.failedStage == nil else {
        throw Failure(description: "\(tag): front end rejected it: \(ref.errors.map { $0.description }.joined(separator: "; "))")
    }
    let expected = Interpreter.run(prog, sema)
    if expected.fault != nil { throw Failure(description: "\(tag): interpreter fault: \(expected.fault!)") }
    if native {
        let n = try runNative(src, tag: tag)
        var e8 = expected
        e8.exitCode = expected.exitCode.map { $0 & 255 }
        if n.text != e8.text || n.exitCode != e8.exitCode {
            throw Failure(description: "\(tag): interpreter disagrees with native clang\n  clang: \(describe(n))\n  interp: \(describe(e8))")
        }
    }
    for v in variants {
        let c = Compilation.compile(src, options: v.options)
        guard let exe = c.executable else {
            throw Failure(description: "\(tag) [\(v.name)]: build failed at \(c.failedStage?.rawValue ?? "?"): \(c.errors.map { $0.description }.joined(separator: "; "))")
        }
        let sim = ISASimulator(exe)
        sim.run(limit: 30_000_000)
        if sim.output != expected {
            throw Failure(description: "\(tag) [\(v.name)]: ISA simulator output differs\n  expected: \(describe(expected))\n  got:      \(describe(sim.output))")
        }
        if verbose { print("  \(v.name): ok, \(sim.instret) instructions") }
        guard v.name == "O0" || v.name == "O1" || v.name == "O1/r3" else { continue }
        // The pipeline must reach exactly the same architectural result.
        for (pname, pc) in pipelineConfigs {
            let p = Pipeline(exe, config: pc)
            p.recordHistory = false
            p.run(maxCycles: 200_000_000)
            if p.output != expected || p.stats.retired != sim.instret {
                throw Failure(description: "\(tag) [\(v.name), pipeline \(pname)]: differs from the ISA model\n  expected: \(describe(expected)) instret=\(sim.instret)\n  got:      \(describe(p.output)) retired=\(p.stats.retired)")
            }
            if verbose { print("    pipeline \(pname): \(p.stats.cycles) cycles, CPI \(String(format: "%.2f", p.stats.cpi))") }
        }
    }
    return expected
}

let args = CommandLine.arguments
let mode = args.count > 1 ? args[1] : "hand"

switch mode {
case "hand":
    var failed = 0
    for (name, src) in handTests {
        do {
            let out = try check(src, tag: name, native: true)
            print("PASS \(name): \(out.text.split(separator: "\n").first ?? "")")
        } catch {
            failed += 1
            print("FAIL \(error)")
        }
    }
    print("\(handTests.count - failed)/\(handTests.count) hand tests passed")
    exit(failed == 0 ? 0 : 1)
case "fuzz", "clang":
    let n = args.count > 2 ? Int(args[2]) ?? 100 : 100
    let seed = args.count > 3 ? UInt64(args[3]) ?? 1 : 1
    var failed = 0
    let t0 = Date()
    for k in 0..<n {
        let s = seed + UInt64(k)
        let src = ProgramGenerator(seed: s).generate()
        do {
            try check(src, tag: "seed\(s)", native: mode == "clang")
        } catch {
            failed += 1
            print("FAIL seed \(s): \(error)")
            if failed >= 5 { break }
        }
        if (k + 1) % 25 == 0 { print("… \(k + 1) programs, \(failed) failures, \(Int(Date().timeIntervalSince(t0)))s") }
    }
    print("\(n - failed)/\(n) random programs agree across \(variants.count) build variants\(mode == "clang" ? " and native clang" : "")")
    exit(failed == 0 ? 0 : 1)
case "show":
    let s = args.count > 2 ? UInt64(args[2]) ?? 1 : 1
    print(ProgramGenerator(seed: s).generate())
case "file":
    let src = try String(contentsOfFile: args[2], encoding: .utf8)
    do {
        let out = try check(src, tag: "file", native: true, verbose: true)
        print(out.text)
    } catch {
        print("FAIL \(error)")
    }
case "asm":
    let src = try String(contentsOfFile: args[2], encoding: .utf8)
    let c = Compilation.compile(src, options: args.count > 3 && args[3] == "O0" ? .o0 : .o1)
    for d in c.diagnostics { print(d) }
    for l in c.asm { print(l.text + (l.note.isEmpty ? "" : "    # " + l.note)) }
default:
    print("usage: spcheck hand | fuzz N [seed] | clang N [seed] | show seed | file x.c")
}
